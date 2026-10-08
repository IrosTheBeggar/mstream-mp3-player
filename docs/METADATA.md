# Metadata: the dual-mode design and the card contract

The player names its tracks from their paths today: the artist is the
first folder under `/music`, the album the second, the title what is left
of the file name. Nothing reads a tag. This document is the design that
brings the tags in, two ways at once:

1. **The transfer software** copies music from an mStream server onto the
   card through mStream's APIs, and writes next to it what it knows about
   each file: the tags, the AutoDJ neighbour table and the album
   thumbnails. It will be part of mstream-terminal (Rust).
2. **The on-device scanner** reads the tags of the files the listener
   copied to the card by hand, which no transfer described.

One builder on the device merges both into the library index. A file
reads the same whichever way it reached the card.

**Status: a design, and its first milestones built (2026-10-07, at
cc13597 on `feature/metadata`, from dev with v0.7.0 released).** N1, the
contract kit, is in `lib/core/CardContract*`, `CardContainer`, `CardTags`,
`CardManifest` and `CardAutoDj`, with the shared fixtures of 2.17 in
`test/fixtures/card/` (made by `tools/card_fixtures.py`, a second
implementation, and frozen). N2, `library.idx` v6 and the builder, is in
`lib/core/LibraryIndex`, `LibraryBuilder` and `NameKey`, host-tested
(3.4.4 says what it decided); the firmware builds v6 from the records
since N10. N3, the queue's remap through `queue.txt`,
is in `lib/core/QueueRemap` (with `QueueModel::release()`, the exact
trim and `queuetext::read()`'s pre-sized blocks), built into
`QueueStore::remap()` and not flashed. The queue's cap of 5,000 tracks
(the user's answer to U12: 3.5, and docs/QUEUE-MODES.md section 15) is
built the same way. N5, the validation walk, is in `lib/core/CardWalk`,
host-tested on fake FAT trees (3.2.6 says what it decided); N10 and N12
bring it into the firmware. N8, the PSRAM sector cache under FatFs, is in
`lib/core/SectorCache`, checked against a reference model and under ChaN's
FatFs on RAM disks (`test/support/fatfs`, host only), on which
`tools/fatmodel.py` counts a lookup and a walk on N11's card (3.2.7 says
what it measured and decided); N10 installs it. N4, the device's own records and their journals, is
in `lib/core/TagStore`, a power cut tested at every step (3.3.7 says
what it decided). N6, the device's tag reader, is in `lib/core/TagScan`
and `TagRules`, checked field for field against a reference reader
(`tools/tagref`, lofty 0.25 with mStream's rules) on the synthetic parity
corpus of 2.17 item 3 (`test/fixtures/tags`, made by
`tools/tag_corpus.py`), its picture anchors read back, its read budget
and its fuzz passes host-tested (3.3.8 says what it decided); core2
builds with it, and N7 and N10 will call it. N9, the names on screen, is
in `lib/core/TrackCatalog`, `LibraryText` and `UiText`, the Library's and
Now Playing's pages, `SleepTimer`'s `kLoose` and the console's `g`
commands (`TagText`, `app/TagConsole`), host-tested (`test_ui_library`,
`test_sleep_timer`) and built, not flashed; with no records on the card
it names everything as before (3.7 says what it decided). N10, the
firmware glue, wires them in: the boot of 3.2.2 (a matching `library.idx`
loaded, a card with records built from them, only a card with none
walked), the validation walk, the compactions and the scan on one card
worker under N7's scheduler, the sector cache under FatFs, the update step
(on the loop until N12), the transfer's thumbnails, streamed covers and
`device.txt`; its portable parts (`lib/core/LibraryBoot`, `CardRoot`,
`CardJobs`) are host-tested, the rest built for `core2` and `core2-dio` and
not flashed (3.8 says what it decided; 6.3.1 is the device batch's
runbook). N11, a synthetic big card for L0 without the real library, is
`tools/synthcard.py`: about 20,000 tiny tagged MP3, FLAC and Opus stubs in
the shape of the user's library (aggregates only; every name made up),
with its tests in `tools/test_synthcard.py` (the stubs read back by its
own parsers and by the repo's, N6's TagScan among them); the user writes
it to a card (6.3's C2). N12, `lib/core/LibraryUpdate`, makes the boot
decision and the update step one portable state machine, host-tested like
`IdlePolicy`:
the build and the save are card-worker steps, the loop goes on behind a
fence on its readers (Now Playing keeps its track's names, the lists say
"Updating the library…"), the queue and Thumbs' pools are lent to the
build and given back, a power cut at any step leaves a card the next boot
reads whole; built for both firmwares with the Output tab's Library row
and its Rescan tags button, not flashed (3.9). Part 2, the card
contract, is a **PROPOSAL (v1)** for the transfer software, whose own
design is still being worked on in mstream-terminal; it is written so
that side can implement it without reading the player's code, and every
place where that side is undecided says so. Part 3 is the player's side,
designed against the code at cc13597; part 5's tag rules bind both
sides. Part 6 answers the
user's question, "can we start without the Core2?": **yes.** About 21.5-29.5
agent-days of the player's work and all of the terminal's (about 10.5-13.5)
are host-only: portable code under `lib/core` with `pio test -e native`,
firmware glue that compiles without a flash, the shared fixtures. The
Core2 is needed for about 4-7 days of measurement and soak, best done as
one batch when it is free.

**In short:**

- **One owner per file.** The transfer software owns `/.mstream/` and the
  music files its committed ledger lists; the device owns `/.player/`;
  everything else under `/music` is the listener's.
- **One record format, two producers.** Both write per-file tag records
  in one binary format (MPTG): the software in `/.mstream/tags-<gen>.bin`,
  the device in `/.player/tags.bin`. Records hold what the file says;
  what to show is the builder's business (part 5).
- **One builder, per file, one record whole:** a transfer record whose
  size and FAT time still match the card's beats a device record that
  matches, which beats the path. A field the chosen record lacks comes
  from the path, never from the other producer.
- **The software reads the tags from the bytes it wrote**, by the same
  rules mStream's scanner uses (lofty 0.25 and the rust-parser's
  selection). mStream's manifest supplies only what the server alone
  knows: the analysed BPM and key, the audio hash, the ids. Its artist
  and album are server-wide agreed spellings, not the file's.
- **Nothing is believed without a check.** Every file carries CRCs; a
  root file (`manifest.bin`) names the companion files of a commit by
  their size and CRC; written aside, then renamed. A card pulled at any
  moment leaves the last commit readable, and anything invalid counts as
  absent: the device then scans for itself, slower, never wrong.
- **The device stops walking the card at boot.** It loads `library.idx`
  when its inputs match and checks the card in the background. At 20,000
  tracks that is about 1.5-3.5 s to a browsable library, against 89-148 s
  for today's walk at every boot (ESTIMATED).
- **AutoDJ** rides on the same commit: a top-K neighbour table
  (`autodj-<gen>.bin`, MPDJ) built by the transfer software from mStream's
  embeddings, keyed by path hash. Files without a row (the listener's own,
  tracks not yet embedded) use "random with filters".

Where the facts come from:

- **The player's code** at cc13597, by file and function. Line numbers
  drift, so they are given only where they help.
- **Two research reports** of 2026-10-06 and 2026-10-07, in the
  scratchpad: `metascan/RESEARCH.md` (the tag scan: the parser prototype
  `metascan/parsing/TagScan.{h,cpp}`, the costs, the MPTG sketch, the
  milestones) and `autodj/RESEARCH.md` (the top-K table, its MPDJ sketch).
  Their labels are kept: **MEASURED** (run, or recorded in the repo's docs
  and logs), **READ** (code or docs), **ESTIMATED** (a model or arithmetic).
  Their library figures are aggregates of the user's library (about
  19,400 playable files in about 2,600 folders, 705 of them directly
  under `/music`); no names.
- **mStream** master 926b97b1 (v6.31.1, schema 78), the version the
  user's server runs; its rust-parser uses lofty 0.25, read at 0.25.1.
- **mstream-terminal** origin/main 2b04dbe (v0.12.1), and the branch
  `claude/gui-mp3-player` (its MP3 Player tab, which flashes firmware).
- **Three design lenses** written for this document (the card contract;
  the device; mStream's API and the terminal), each checked against the
  code above. Where they disagreed, section 9 says what this takes and
  why.
- No device and no SD card were used.

**What the user decided** (2026-10-07):

- Metadata is dual mode, as above. The transfer software lives in
  mstream-terminal and uses mStream's APIs; its design is in progress.
- The tag-scan research's plan and its defaults:
  - **Stage A first:** albums grouped by folder, shown with their tag
    names, tracks ordered by disc and track number, the article sort,
    lengths;
  - **the tag wins for display**; the Folders view stays raw;
  - **Artists** lists folder artists in Stage A (album artists come with
    Stage B's grouping);
  - **an artist's albums newest first**; **no Genres view** for now;
  - **the scan runs on battery and while playing**, yielding to track
    changes and to the UI;
  - **covers:** the folder image first, embedded JPEG as the fallback, no
    PNG or progressive JPEG on the device;
  - **Opus tags** are read;
  - **the transfer software writes per-file tag records**, not a built
    index;
  - the cheap file-name fixes are done (0.7.0: "The" sorts under its next
    word, disc-track and artist-number names, the artist prefix off
    titles).
- The proposal the user saw and did not object to: one owner per file;
  one builder that merges per file; change detection by generation, the
  walk, and size, time and fingerprint; atomic writes; AutoDJ rows keyed
  by path hash, with "random with filters" for files that have none.
- **The user's answers to part 7** (2026-10-07): the device defaults
  stand: a transfer record wins while it still matches (U8); hand-copied
  files show at once with their file names (U11); the scan stops below
  10% battery (U13); a host test may vendor FatFs (U14). U12 is replaced
  by **a cap on the queue, 5,000 tracks** (3.5; built: QUEUE-MODES.md
  section 15). A spare card is ready for N11's synthetic 20k card and
  later tests (U1).

**What this design decided** (the defaults didn't say; each is open to
change, and part 7 lists the questions):

- The tag records of the transfer software are read from the card file,
  not taken from the manifest (section 2.7, section 9 item 3).
- Records keep every value a file holds (lists separated by U+001F); the
  display join, the votes and the article rule are the builder's
  (part 5).
- The default card layout drops mStream's vpath, so the device's
  artist/album depths hold (section 2.8.6).
- The transfer thumbnail's source follows the user's folder-first rule,
  so it can come first on the device; a cover the listener adds by hand
  still wins (section 2.14).

---

## 1. The dual-mode model

### 1.1 Who writes what

```
  mStream server
      |  sync/manifest, metadata/batch, /media, /transcode,
      |  /album-art, discovery/local/embeddings
      v
  transfer software (mstream-terminal, on a PC; later the device's WiFi sync)
      |  writes music files it owns, and /.mstream/
      v
  +--------------------------- the SD card (FAT32, MBR) ---------------------------+
  |  /music/...    audio and images: the software's files and the listener's own   |
  |  /.mstream/    manifest.bin -> tags-<gen>.bin, autodj-<gen>.bin, thumbs/        |
  |  /.player/     device.txt, tags.bin, library.idx, queue.txt, thumbs/, ...      |
  +--------------------------------------------------------------------------------+
      ^  the listener's PC: copies files under /music by hand
      |
  Core2:  walk /music --> scanner (only files no record covers) --> /.player/tags.bin
          builder( transfer records T + device records D + the walk + the names )
              --> /.player/library.idx --> the Library, Now Playing, the queue
```

- **The transfer software** writes its music files, the per-file records
  (tags and its ledger), the AutoDJ table and the album thumbnails, all
  under one commit (section 2.5). Its records cover exactly the files it
  wrote.
- **The device** reads `/.mstream/` and never writes it (except, later,
  as the WiFi sync agent, which follows the same protocol: section
  2.12.4). It scans the files no fresh record covers, and keeps its own
  records in `/.player/tags.bin`.
- **The listener** can do anything to the card on a PC: add an album,
  retag a file, delete `/.mstream` or `/.player`. Both sides must survive
  it.

### 1.2 What each card looks like to the device

| The card | Records | What the device does |
|---|---|---|
| Filled by the transfer software only | T covers every file | No scan. A build from T after each transfer, a background walk to confirm. |
| Filled by hand only (a card reader, no software) | none | Today's walk once into a path-named index, then the scan in the background; the names improve as it goes. |
| Both: a transfer, plus albums the listener added | T for the software's files, D for the rest | The scan reads only the listener's files. |
| A software file the listener retagged on a PC | T no longer matches (size or time changed) | The scan reads it and D wins; the next transfer leaves the file alone, drops it from its ledger and reports it (section 2.10.2). |
| `/.mstream` deleted by the listener | none from T | The scan reads everything the old T covered. |
| `/.player` deleted | T still valid | A build from T; the listener's files are scanned again. |
| Damaged or newer-format `/.mstream` files | T absent | As if no transfer had happened. |

### 1.3 The invariants

1. **One owner per file** (section 2.11). Nobody writes a file it doesn't
   own; the software deletes only files its committed ledger lists *and*
   that still match it.
2. **One record per file, whole.** The builder takes one producer's record
   for a file, and fills its gaps from the path, never from the other
   producer. A file's names come from one place.
3. **Records describe the file, not the library.** The transfer record is
   what the reference rules read from the card file (part 5), the device
   record what the device reads from the same bytes by the same rules.
   Votes, joins and sorting happen once, in the builder.
4. **Nothing is believed on its word.** Sizes, FAT times, CRCs and
   generations are checked; a file that fails is absent, never an error
   the listener must clear.
5. **Every producer records every field it can read**, even fields
   today's index doesn't use. A new index field is then a rebuild
   (seconds), not a rescan (minutes).

---

## 2. The card contract (PROPOSED, v1)

What the transfer software and the player keep on the SD card, byte for
byte, so two independent implementations agree: the transfer software in
mstream-terminal (Rust) and the Core2 firmware (C++). The transfer
software's design is still being worked on: this is a proposal for it to
adopt or answer, not a description of shipped code. Section 2.17's
fixtures and 2.18's vectors are what make "agree" testable without a
device.

### 2.1 Who, and the words used

- **The software** is the transfer software in mstream-terminal. A future
  WiFi sync on the device takes the same role and follows the same rules
  (section 2.12.4).
- **The device** is the player's firmware.
- **The listener** is the person who owns the card.
- **T** is the transfer software's tags file of the current commit; **D**
  is the device's own (`/.player/tags.bin`); **the walk** is the device's
  listing of `/music`.
- MUST, SHOULD and MAY are used as in RFC 2119. "Reader" and "writer" mean
  any implementation on either side.

### 2.2 The card at a glance

The card is FAT32 (or FAT16) on an MBR partition: the firmware's FatFs is
built without exFAT and without GPT (`lib/core/CardFormat`). The tree:

```
/                               the volume root
├─ music/                       the library: audio and images, up to 8 folder levels
│   └─ Artist/Album/01 - Title.mp3
├─ .mstream/                    OWNED BY THE SOFTWARE
│   ├─ manifest.bin             MSMF: the commit point (a few KB)
│   ├─ manifest.tmp             only while a commit is being written
│   ├─ tags-<gen>.bin           MPTG, source 2: the tag records and the ledger
│   ├─ autodj-<gen>.bin         MPDJ: the AutoDJ neighbour table (optional)
│   ├─ pending.bin              MSPD: an unfinished run's plan (only while one is; written through pending.tmp)
│   ├─ stage/<8 hex>.tmp        files being downloaded
│   ├─ thumbs/<h>/<8 HEX>.565   MPTH v1: album thumbnails
│   └─ (any other name)         the software's private files; the device ignores them
└─ .player/                     OWNED BY THE DEVICE
    ├─ device.txt               what this firmware reads (the software reads it)
    ├─ tags.bin                 MPTG, source 1: the device's records
    └─ tags.jnl, walk.jnl, library.idx, queue.txt, opus.idx, thumbs/ ...
                                device-internal; may change in any firmware
```

- `<gen>` is the writer's generation as 8 lowercase hex digits
  (`tags-0000002a.bin`).
- File names in `/.mstream/` are lowercase. FAT matches names
  case-insensitively, but writers MUST use these spellings.
- The software MAY keep private files in `/.mstream/` under names this
  contract doesn't define (for example the selection and its settings as
  `state.json`; part 7, T3). The device never reads them.
- The device's library walk starts at `/music`. It never sees `/.mstream`
  or `/.player`, and it skips every name under `/music` that starts with
  a dot (`src/storage/LocalStorage.cpp`, `walk()`).

### 2.3 Conventions shared by every file

#### 2.3.1 Bytes

- Every integer is **little-endian** and unsigned unless written `iNN`.
- **Identifiers come in two kinds,** and each is stored one way only:
  - **Computed as integers:** the FNV-1a 64 values (path hashes, qfp,
    artistKey, serverUrlKey) and the random ids (cardId, commitId,
    runId). Stored as little-endian integers, and sorted and compared as
    unsigned integers: B1F7E69FBD466B59 is the bytes
    `59 6B 46 BD 9F E6 F7 B1`.
  - **Byte strings:** MD5 digests (`hash`, `audio-hash`), the SHA-256
    prefixes (COMP `key`, MPDJ `selectionSig`), DJRW `hashPrefix` and
    UUIDs (`serverInstance`). Stored as their bytes in the order of their
    hex text, RFC 4122 order for a UUID, never parsed into an integer
    (not as a Windows GUID, not as a u64), and compared byte by byte
    (`memcmp`).
- Thumbnail pixels are big-endian RGB565, as the device stores them
  today.
- A fourcc is 4 ASCII bytes in reading order: "MPTG" is the bytes
  `4D 50 54 47`, which reads as the u32 0x4754504D.
- Reserved fields and padding MUST be written as zero and MUST be ignored
  by readers.

#### 2.3.2 Checksum: CRC-32

- CRC-32/ISO-HDLC, as zlib, PNG and Rust's `crc32fast` compute it:
  reflected polynomial 0xEDB88320, initial value 0xFFFFFFFF, final XOR
  0xFFFFFFFF.
- **Check value:** the CRC-32 of the ASCII bytes `123456789` is
  **0xCBF43926**.
- The Ogg CRC in the player's `lib/core/OggPage` is a different CRC and
  MUST NOT be used here.

#### 2.3.3 Path hash: FNV-1a 64

- **The algorithm:** offset basis 0xCBF29CE484222325, prime
  0x00000100000001B3, byte by byte (`h ^= b; h *= prime`, mod 2^64).
- **What it hashes:** the *absolute card path* as UTF-8 bytes: the literal
  `/music`, then `/` and each name.
  - The `/music` prefix is always lowercase, whatever case the card
    stores that folder in.
  - Every name below it is spelled **exactly as the card's directory
    stores it**: the stored UTF-16 long name converted to UTF-8, which is
    what FatFs returns (section 2.8.5).
  - No trailing slash, no NUL.
- **A folder's hash** is the same over its path: `/music` for the root,
  `/music/Artist/Album` for an album.
- **It is the device's hash already.** The device's thumbnail files key on
  the FNV-1a 64 of the cover's card path (`thumbfile::pathHash` over the
  index's `"/music/.../cover.jpg"`), and the Opus open cache uses the same
  function.

| Input | FNV-1a 64 |
|---|---|
| `` (empty) | CBF29CE484222325 |
| `a` | AF63DC4C8601EC8C |
| `/music` | 75DC8A6A38687865 |
| `/music/Artist/Album` | B1F7E69FBD466B59 |
| `/music/Artist/Album/01 - Title.mp3` | DA70B312760188D8 |
| `/music/Café/Album/01 - Title.mp3` (é as NFC, `C3 A9`) | F1B24FC757494F2B |
| the same with é as NFD (`65 CC 81`) | 57017C0CC011BB1B |

The last two rows are why the software records names as the card stores
them: the same text in two normalisation forms has two hashes, and not
every PC lists the stored form unchanged (2.8.5 says how each OS's
listing is brought back to it).

#### 2.3.4 FAT timestamp, and the uniform-skew rule

- **The value:** `fatTime` is a u32, `(fdate << 16) | ftime`: the raw FAT
  directory fields as FatFs's `FILINFO` reports them.
  - fdate: bits 15-9 the year minus 1980, bits 8-5 the month, bits 4-0
    the day.
  - ftime: bits 15-11 the hour, bits 10-5 the minute, bits 4-0 the
    seconds divided by 2.
- **0** means unknown. 0 is also what some tools leave in a directory
  entry, so the two read alike.
- **Invalid stamps:** a stamp is invalid when its month is 0 or above 12,
  its day is 0 or past its month's last day, its hour is 24 or more, its
  minute 60 or more, or its sec2 30 or more.
- **A recorded or observed 0, or an invalid stamp, never matches by
  time:** qfp decides (2.3.5), and the pair is left out of the skew
  histogram below. W is defined only for valid stamps.
- **Example:** 2026-10-07 14:30:42 is **0x5D4773D5**; one hour later is
  0x5D477BD5.

FAT stores local wall-clock time with no time zone, at 2 s resolution.
The device reads the raw fields. A PC's operating system converts them to
and from UTC, so the software MUST turn what the OS reports back into the
raw fields:

- **Windows:** the FAT driver converts with the time zone bias in force *at
  the moment of the call*. Read the modification time, convert it with
  that same current bias (`FileTimeToLocalFileTime`, not a DST-aware
  conversion of the stamp's own date), then encode the fields. This
  recovers the raw fields exactly, before and after a DST change.
- **Linux (vfat):** the kernel applies the mount's `time_offset=` or
  `tz=UTC` option, else the kernel's `sys_tz`. Read the offset the mount
  uses, apply it, then encode.
- **macOS and others:** use the platform's local offset; the skew rule
  below and the fingerprint (2.3.5) cover any error.

Further rules:

- The software MUST read `fatTime` back from the card *after* the file is
  in its final place. It MUST NOT compute it from the source's time: FAT
  rounds, copies may keep or change the time, a rename on FAT keeps it.
- The software MAY set the file's modification time first (to the
  server's `modified`, say); what it records is still the value read back.
- **Every `fatTime` in one tags file MUST come from listings made in one
  run** of the software. It re-lists the card at each run rather than
  copying stamps from the previous ledger, so any conversion error is
  uniform across the file.

**The uniform-skew rule (both sides).**

1. Take the pairs (recorded, observed) of the files in one tags file whose
   sizes match and whose two stamps are both valid and non-zero.
2. Convert each stamp to wall seconds, with no time zone:
   `W = days_since_1980_01_01 × 86400 + hour × 3600 + minute × 60 + 2 × sec2`.
3. Let Δ = W(observed) − W(recorded) for each pair.
4. Let D be the most frequent non-zero Δ (ties: the smaller |D|, then the
   negative one). D is the tags file's **skew** when all of these hold:
   - at least 8 pairs have Δ = D;
   - they are at least half of all the pairs;
   - |D| ≤ 86,400;
   - D is a multiple of 900 (time zones run in quarter hours).
5. Then a file's time matches when its Δ is 0 or D.

The device keeps the skew it found per (cardId, generation, commitId).
The software applies the same rule to its previous ledger at the start of
a run. A non-uniform error (macOS converting each stamp with its own
date's DST, say) falls to the fingerprint: slower, never wrong.

The rule's answer doesn't depend on the order the pairs come in, and an
implementation that counts in bounded memory MUST give the same answer.
The player's (`SkewHistogram`, 2 KB) keeps 256 deltas exactly, and past
256 distinct ones becomes a Misra-Gries summary, which still holds any
delta that half the pairs have; one more pass over the pairs then counts
its candidates exactly (3.2.3). Without that pass it may find no skew
where the rule finds one, never another skew.

#### 2.3.5 Quick fingerprint (qfp)

- **The value:** a u64, the FNV-1a 64 (2.3.3) of three parts in order:
  1. the file's size as a u64, little-endian (8 bytes);
  2. the head: bytes `[0, min(size, 4096))`;
  3. the tail: bytes `[max(4096, size − 4096), size)`, empty when
     size ≤ 4096.
- The head and the tail never overlap. It costs two reads.

| Content | qfp |
|---|---|
| 0 bytes | A8C7F832281A39C5 |
| 100 bytes, byte i = i & 0xFF | B708DC48BA0A842D |
| 4,096 bytes, same pattern | 636A94FE9C19DC15 |
| 5,000 bytes, same pattern | C8E651224ADA889C |
| 10,000 bytes, same pattern | F17B194EF7F5F338 |

**What it is for:** confirming a file whose size matches and whose time
doesn't (a non-uniform skew, a recovery after a pulled card); the
software's optional deep check; a device "Verify card" command. It
catches a retag at either end of the file, not one in the middle: about
5% of MP3s keep their text frames behind a large picture (MEASURED,
metascan section 5.1). No cheap check sees every retag.

#### 2.3.6 Text

- All text is UTF-8 with no NUL bytes.
- **Tag text is stored as decoded, by neither side normalised** (no NFC,
  no trimming beyond part 5's rules): two producers reading the same
  bytes MUST write the same bytes. Comparing and sorting fold later, in
  the builder.
- **Lists:** a field with several values separates them with U+001F (the
  unit separator, as mStream's `genres_concat` does). Within a list,
  byte-identical repeats are dropped, first occurrence kept.
- **Control characters:** writers replace U+0000-U+001F and U+007F inside
  a value with U+0020. A value empty after that is dropped.
- **Limits:**
  - one value: at most 255 bytes, cut at a code-point boundary, and the
    record's TRUNCATED flag set;
  - one list: at most 16 values and 1,023 bytes, **the U+001F separators
    counted**. The values are taken in order, each already cut to 255
    bytes, and the **first value that would pass either limit ends the
    list**: it and every value after it are dropped, and TRUNCATED set.
    (Five 204-byte values keep four: 4 × 204 + 3 = 819, and a fifth makes
    1,024.)
  - readers MUST accept values up to these limits, and MAY cut them
    further for display.
- **The steps run in this order:** control characters to spaces; empty
  values dropped; each value cut to 255 bytes; repeats dropped; the list
  limits.
- **A single-valued field** (title, album, the sort names, the
  MusicBrainz ids: 2.6.5) is the first value left after the empty ones
  are dropped, cut to 255 bytes; the values after it aren't the field's
  and never set TRUNCATED. (A value empty after its control characters
  became spaces isn't empty: a lone tab is stored as one space.)
- **Paths:** names are stored as the card stores them (2.8.5); the names
  the software *creates* are NFC (2.8.3).

### 2.4 The container (MSMF, MPTG, MPDJ, MSPD)

Every binary file in this contract except the thumbnails has one frame: a
header, a section directory, then the sections, each 8-byte aligned.

#### 2.4.1 Header (the common part, 40 bytes)

| Off | Size | Field | Meaning |
|---|---|---|---|
| 0 | 4 | magic | fourcc: `MSMF`, `MPTG`, `MPDJ` or `MSPD` |
| 4 | 2 | major | incompatible format version (1) |
| 6 | 2 | minor | compatible additions (0) |
| 8 | 4 | headerBytes | the whole header (common + type-specific), a multiple of 8; the directory starts here |
| 12 | 4 | sectionCount | directory entries, ≤ 64 |
| 16 | 4 | fileBytes | the file's exact length |
| 20 | 4 | generation | the writer's generation (2.5.2) |
| 24 | 8 | cardId | the card's id (2.5.2); 0 in files written before a card has one |
| 32 | 4 | headerCrc | CRC-32 of bytes `[0, headerBytes + 32 × sectionCount)`, computed with this field set to 0 |
| 36 | 4 | reserved | 0 |
| 40 | … | type-specific fields | up to headerBytes |

#### 2.4.2 Section directory (32 bytes per entry, right after the header)

| Off | Size | Field | Meaning |
|---|---|---|---|
| 0 | 4 | type | fourcc |
| 4 | 4 | flags | bit 0 REQUIRED: a reader that doesn't know this type MUST treat the file as absent |
| 8 | 4 | offset | from the file's start; a multiple of 8 |
| 12 | 4 | bytes | the section's length |
| 16 | 4 | count | records (0 for a blob) |
| 20 | 4 | stride | bytes per record (0 for a blob); for an array, bytes = count × stride |
| 24 | 4 | crc | CRC-32 of the section's bytes |
| 28 | 4 | reserved | 0 |

Layout rules: sections are in increasing offset order and don't overlap
(a section of 0 bytes, such as an empty RECS, shares its offset with the
next one); the gaps between them are zero padding of under 8 bytes; the
last section ends exactly at fileBytes; a type appears at most once per
file.

So that two writers produce the same bytes:

- **Order.** Sections are written in the order of their format's section
  table (2.5.3, 2.6.2, 2.12.5, 2.13.2), and the directory lists them in
  that order. The first starts right after the directory (at headerBytes
  + 32 × sectionCount); each next one at the previous one's end, rounded
  up to 8.
- **Presence.** A REQUIRED section is always present, even with no
  records. An optional section is present exactly when its format's
  table says so, and is never written empty.
- **The values a writer would otherwise choose** (cardId, commitId,
  runId, the times, the generation) come from the caller; the
  conformance fixtures supply them (2.17).
- The device's private sections in `/.player/tags.bin` (3.3.2) follow
  the contract's sections.

#### 2.4.3 Validation

A reader MUST treat a file as **absent** when any of these fails:

- **the frame:** the magic; a supported major; headerBytes a multiple of
  8 and at least its format's v1 header; at most 64 sections; headerCrc;
  fileBytes equal to the length on the card;
- **the directory:** 2.4.2's layout rules (offsets multiples of 8 in
  increasing order, the first at the directory's end, each next one under
  8 bytes past the end of the one before, the last ending at fileBytes, no
  type twice); no REQUIRED section of a type the reader doesn't know
  (2.4.5); every section its format requires present; for each known
  array, a stride at least its v1 stride (a later minor's longer rows are
  read by their known prefix) and bytes = count × stride; for each known
  blob, count and stride 0;
- **the header's own fields:** the counts it states equal its sections'
  (MPTG recordCount and folderCount, MPDJ rowCount and pathCount), and
  2.4.5's values that make a file absent (MPTG source, MPDJ scoreKind);
- the CRC of each section the reader uses;
- the bounds checks of the section rules below.

**The structure the device relies on** is checked too, because a writer
bug can produce valid CRCs over a wrong order, and the builder is a
streaming merge that assumes canonical order and follows `FOLD.parent`.
In the sections a reader uses, each of these MUST hold, or the file is
absent:

- **FOLD:** folder 0 is there (folderCount is at least 1), its parent is
  0xFFFFFFFF and its name offset is 0; every other folder's parent is
  lower than its own index, and is the folder just before it or one of
  that folder's ancestors (strict pre-order, checked with a stack);
  siblings' names strictly increase by bytes (2.6.8).
- **firstRecord** of each folder equals the number of records whose
  folder index is lower.
- **RECS:** folder indexes are below folderCount; records strictly
  increase by (folder index, name bytes).
- **Names** (folders and records) are non-empty, contain no `/`, and
  are not `.` or `..`.
- **Paths:** `/music/` and the path of every folder and record (and every
  LIBR root and PEND path) is at most 255 bytes of UTF-8 (2.8.2; the
  software keeps to it by 2.8.3, step 5, the device by its walk), so a
  reader's path buffers and its folder stack are bounded (at most 124
  levels).
- **Strings** (2.4.4): every offset a reader uses is inside its section
  with a NUL after it inside, and the strings a reader uses lie in
  2.4.4's order: each starts at or after the end of the one before it in
  that order (gaps are allowed: a later minor's strings fill them). So a
  reader reads each string section front to back, in one pass; the
  builder reads STRS as two runs at once (the producer and the folder
  names; then, from the first record's name on, the records' names and
  runs) and combines their two CRCs into the section's.
- **HIDX**, when used: present when recordCount > 0 (2.6.2); recordCount
  entries, strictly increasing by (pathHash, record), each record once,
  each pathHash equal to its record's path hash. A streaming reader MAY
  check the last two against the records' own (pathHash, record) pairs
  through an order-free 128-bit digest, as the player's does: with the
  strict order and the count, equal digests mean each record once with
  its own hash, all but certainly.
- **ORIG:** its count equals recordCount (2.6.6).
- **COMP:** exactly one `MPTG` entry, then at most one `MPDJ` (2.5.3);
  an entry of another kind is ignored wherever it stands (2.4.5); each
  name matches `^tags-[0-9a-f]{8}\.bin$` for MPTG or
  `^autodj-[0-9a-f]{8}\.bin$` for MPDJ, and its 8 digits are the entry's
  generation.
- **LIBR:** each root a relative path made of names (as above), the
  roots strictly increasing by bytes.
- **MPDJ:** indexBytes is 2 or 4, and 2 exactly when rowCount is 65,535
  or less; k is at least 1; DJNB's stride is k × (indexBytes + 1);
  scoreKind is 1 (2.4.5); DJRW rows are in non-decreasing hashPrefix
  order (2.13.2); DJPH strictly increases by (pathHash, row); every DJPH
  row and every DJNB index is below rowCount, except a DJNB unused slot's
  all-ones index.
- **MSPD:** entries are in 2.12.5's order, each group strictly increasing;
  an op of 0 makes the plan absent; paths are non-empty, relative and
  made of names (no empty, `.` or `..` component), so no plan can name a
  file outside `/music`. (An op above 3 doesn't make the plan absent: it
  stops the software, 2.4.5, and isn't placed in the order.)

Checks that need a whole section MAY run while the section streams; a
failure found mid-stream makes the file absent from then on (for a build
from T: 3.4.2 restarts it without T). Each check is on 2.17's hardening
list.

An absent file is never an error the listener must clear: each side falls
back as section 2.9 says. The device MAY skip a large section's CRC on
later boots once it has checked that same file (by headerCrc and
generation) and noted it in `/.player`.

#### 2.4.4 String sections (`STRS`, `OSTR`)

- A blob of NUL-terminated UTF-8 strings.
- Byte 0 is NUL, so **offset 0 is the empty string** and means "absent".
- Every referenced offset MUST be inside the section, with a NUL after it
  inside the section.
- Strings are **not shared**: each reference gets its own copy (a shared
  copy would save about 0.4 MB at 20k tracks; byte-identical output from
  two writers is worth more). An empty string is offset 0 and is never
  written.
- **The order:** the leading NUL; then the strings the header names, in
  header-field order; then the format's own:
  - MSMF: `producer`, `serverRevision`; then the COMP names in COMP
    order; then the LIBR roots in LIBR order;
  - MPTG: `producer`; then 2.6.8's order;
  - MPDJ: `modelId`, `modelVersion`, `metric`, `license`, `attribution`;
    nothing else;
  - MSPD: the PEND paths in PEND order.
- `OSTR` follows 2.6.8.
- Readers check that the strings they use come in this order (2.4.3): a
  writer that shared or reordered strings makes its file absent.

#### 2.4.5 Versions and compatibility

- **Major.** A reader supports a set of majors (v1 readers: {1}); a file
  of another major is absent to it. The device then behaves as if the file
  weren't there. The software MUST NOT write to a card where **any**
  contract-named file in `/.mstream` (`manifest.bin`, `manifest.tmp`,
  `pending.bin`, `pending.tmp`, `tags-*.bin`, `autodj-*.bin`) carries a
  known magic with a major it doesn't know. It checks the magic and the
  major alone, before any CRC, so a newer writer's half-finished commit
  stops it too; it reports the card read-only and asks for an update.
- **Minor.** Readers accept any minor.
  - New fields go only at the **end** of a record (a larger stride) or of
    the header (a larger headerBytes).
  - A reader reads the prefix it knows and skips the rest; reading a
    shorter record than it knows, it treats the missing tail as zeros.
  - So **every field added in a minor version MUST use all-zero bytes to
    mean "absent or unknown"**.
  - **Every minor addition MUST be re-derivable from the card and the
    server.** An older writer that carries a file forward drops what it
    doesn't know, and the next newer run derives it again. A fact that
    can't be re-derived (a listener's answer, say) needs a new major, or
    the software's private state.
- **New sections** are skipped by readers that don't know them, unless
  flagged REQUIRED.
- **New flag bits** are ignored by older readers. Writers set only bits
  they define.
- **New enum values:** where 0 has a meaning of its own, "read it as 0"
  would be wrong, so each field says what an unknown value means:

| Field | An unknown value reads as |
|---|---|
| RECS `container` (4-254) | 0, unknown (the device goes by the extension anyway) |
| RECS `camelot` (above 24) | 0, none |
| RECS `picMime` (above 3) | 3, other: not decoded |
| RECS `picCoding` (above 3) | no picture: picOffset and picLength ignored |
| RECS `flags` compilation (3) | 0, not said |
| MPTG `source` (0, or above 3) | the file is absent |
| MPDJ `scoreKind` (not 1) | the file is absent |
| MSMF COMP `kind` | the entry is ignored |
| MSPD `op` (above 3) | the software MUST NOT write, as for an unknown major |
| ORIG `convertedTo` (not a container code) | a conversion of unknown kind: not VERIFIED, never adopted |
- **These need a new major:** changing a field's meaning, size or
  position; an ordering rule readers rely on; a new REQUIRED section that
  older readers can't skip safely.
- **Writers** write the newest minor they know, in the newest major both
  sides read (the device lists its majors in `/.player/device.txt`,
  2.15).

### 2.5 `manifest.bin` (MSMF): the commit point

A small file that says which companion files make up the card's current
state. Writing it, through `manifest.tmp`, is the moment a run's work
becomes visible. The per-file ledger lives in the `ORIG` section of the
tags file it names (2.6.6), so paths are not stored twice and the device
reads the root with one small read at every boot.

#### 2.5.1 Header (type-specific part; headerBytes = 96)

| Off | Size | Field | Meaning |
|---|---|---|---|
| 40 | 4 | commitTime | Unix seconds (UTC) by the writer's clock; informational |
| 44 | 4 | flags | bit 0 FINAL: this commit ended a run (clear: a checkpoint inside one) |
| 48 | 8 | commitId | a random u64, new for every commit |
| 56 | 16 | serverInstance | the mStream server's instance UUID, 16 bytes in RFC 4122 order (2.3.1); zeros: unknown (part 4, A6) |
| 72 | 4 | producer | STRS: the writer and its version, `mstream-terminal 0.13.0` |
| 76 | 4 | serverRevision | STRS: the sync manifest's `revision` (its ETag) when the selection was read; "" unknown |
| 80 | 4 | baseGeneration | the generation this commit replaced (0 for the first) |
| 84 | 4 | reserved | 0 |
| 88 | 8 | serverUrlKey | the FNV-1a 64 of the server's normalised base URL (below); 0 unknown |

**The server's identity** (used by 2.11):

- **serverInstance** is A6's id when the server gives one.
- **serverUrlKey** is always written in v1, so the guard works before A6
  ships. The URL is normalised first: the scheme and the host in lower
  case; no user name, password, query or fragment; an empty port, or the
  default one (80 for http, 443 for https), dropped, any other written in
  decimal without leading zeros; the path kept, every trailing slash
  removed. `HTTP://Music.Example:3000/` becomes `http://music.example:3000`,
  whose key is ED901EA3EE763AC7. Only the hash is on the card, never the
  URL or a token.
- The same server reached through two URLs (the LAN and a tunnel, say)
  has two keys. The listener's "same server" answer (2.11) is kept in the
  software's private state, as a list of keys known to be this card's
  server.

#### 2.5.2 Generation, commit id, card id

- **generation** counts commits on this card. The first is 1; each commit
  is the previous root's generation + 1. If no valid root exists but
  generation-named files do, the next generation is one more than the
  largest found: a name is never reused. Every writer (the terminal, the
  device's future sync) counts from the same root.
- **commitId** tells two commits of the same generation apart (a restored
  backup of `/.mstream`, say).
- **cardId** is a random u64 made at the first commit and kept from then
  on. Every file the software writes carries it.
- The device keys everything it derives from T on the triple **(cardId,
  generation, commitId)**.

#### 2.5.3 Sections

| Type | Flags | Stride | Content |
|---|---|---|---|
| `COMP` | REQUIRED | 40 | the commit's companion files |
| `LIBR` | | 4 | library roots: STRS offsets of folders relative to `/music` (2.8.6), sorted by their bytes; present exactly when there is a root other than `/music` (absent: one root, `/music`) |
| `STRS` | REQUIRED | blob | strings |

`COMP` entry (40 bytes):

| Off | Size | Field | Meaning |
|---|---|---|---|
| 0 | 4 | kind | the companion's fourcc: `MPTG` or `MPDJ` |
| 4 | 4 | generation | the companion's own header generation (older than the root's when a file was reused) |
| 8 | 4 | fileBytes | its exact length |
| 12 | 4 | headerCrc | its headerCrc |
| 16 | 4 | name | STRS: its file name inside `/.mstream/` (`tags-0000002a.bin`) |
| 20 | 4 | reserved | 0 |
| 24 | 16 | key | MPDJ: the selection signature (2.13.3); zeros for MPTG |

- A root MUST list exactly one MPTG companion (source 2 or 3), and at most
  one MPDJ, in that order. Each name is `tags-<gen>.bin` or
  `autodj-<gen>.bin` with the entry's own generation (2.4.3).
- A companion is valid only when **all** of these hold: the file exists;
  it validates (2.4.3); its header's generation, fileBytes and headerCrc
  equal the entry's; for an MPDJ, the selection signature in its header
  equals the entry's key.
- A companion that fails is absent. The root stays valid.

#### 2.5.4 Which root counts

Readers look at both `manifest.bin` and `manifest.tmp`:

1. Validate each (2.4.3).
2. If both are valid, the **higher generation** wins; on equal
   generations, `manifest.bin`.
3. If only one is valid, it wins. If neither, the card has no transfer
   data.

A fully written `manifest.tmp` is a finished commit even if its rename
never happened: every companion it names was complete before its first
byte was written. The device never renames or deletes these files; the
software's next run finishes the rename (2.12.1, step 1).

Two valid roots with the same commitId and headerCrc are one commit seen
twice: a rename cut between its two directory writes (2.12.1, "Cut
renames"). Readers use either. The two may share one cluster chain, so
the software deletes neither until a disk check has run.

### 2.6 Tags files (MPTG v1)

One format for both producers:

- `/.mstream/tags-<gen>.bin`, source 2 (or 3, the device as the sync
  agent): the software's records and its ledger;
- `/.player/tags.bin`, source 1: the device's records. It MAY carry
  device-private sections (section 3.3.2), which other readers skip.

#### 2.6.1 Header (type-specific part; headerBytes = 72)

| Off | Size | Field | Meaning |
|---|---|---|---|
| 40 | 1 | source | 1 the device's scan, 2 the transfer software, 3 the device as the sync agent |
| 41 | 1 | reserved | 0 |
| 42 | 2 | parserVersion | the producer's own reader version (the device rescans records of an older version of its own parser); 0 unknown |
| 44 | 2 | readRules | the version of part 5's reading rules the records follow (1) |
| 46 | 2 | reserved | 0 |
| 48 | 4 | recordCount | = RECS count |
| 52 | 4 | folderCount | = FOLD count |
| 56 | 4 | albumValues | distinct album values, for pre-sizing |
| 60 | 4 | artistValues | distinct artist and album-artist values, for pre-sizing |
| 64 | 4 | producer | STRS: the writer and its version |
| 68 | 4 | reserved | 0 |

- **Every record in one file was read under the header's parserVersion
  and readRules.** A software whose reader or rules version changed reads
  every record it carries forward again from the card file (2.10.2)
  before it writes the next tags file; it never mixes versions in one
  file.
- **albumValues and artistValues** are always computed by writers: the
  number of distinct values by bytes over every record's list items
  (artistValues counts the artist and album-artist items together).
  Readers use them only to pre-size; a wrong count costs memory, never
  correctness.

#### 2.6.2 Sections

| Type | Flags | Stride | Content |
|---|---|---|---|
| `FOLD` | REQUIRED | 16 | the folder table |
| `RECS` | REQUIRED | 72 | one record per file |
| `STRS` | REQUIRED | blob | names and tag strings |
| `HIDX` | | 12 | the path-hash index; present exactly when recordCount > 0 |
| `ORIG` | | 80 | the ledger, one row per record (the device ignores it); present exactly when the source is 2 or 3 and recordCount > 0 |
| `OSTR` | | blob | the ledger's strings (server paths), apart so the device can skip them; present exactly when ORIG is |

#### 2.6.3 `FOLD`: folders (16 bytes)

| Off | Size | Field | Meaning |
|---|---|---|---|
| 0 | 4 | parent | folder index; 0xFFFFFFFF for folder 0 |
| 4 | 4 | name | STRS: the folder's name as the card stores it (folder 0: offset 0) |
| 8 | 4 | flags | bit 0 OWNED: the producer created this folder (sources 2 and 3); bit 1 THUMB: `/.mstream/thumbs` has this album folder's thumbnail (2.14.1) |
| 12 | 4 | firstRecord | the number of records whose folder index is lower, so its records run to the next folder's firstRecord (or recordCount), and an empty folder's equals the next one's |

**Folder 0 is `/music`**, and is always there, even in a file with no
records. The table holds every ancestor of a record, and every OWNED
folder, even an empty one, so the software can remove it later, with its
ancestors (a parent the listener made stays in the table, without
OWNED), and no other folder. A folder's path is `/music` followed by `/`
and each folder's name from the root down.

#### 2.6.4 `RECS`: records (72 bytes)

| Off | Size | Field | Meaning (absent value) |
|---|---|---|---|
| 0 | 4 | folder | FOLD index |
| 4 | 4 | name | STRS: the file name as the card stores it |
| 8 | 4 | strings | STRS: this record's string run (2.6.5); 0 none |
| 12 | 4 | size | the card file's size in bytes (FAT32 files are < 4 GiB) |
| 16 | 4 | fatTime | 2.3.4 (0 unknown) |
| 20 | 4 | durationMs | length in ms (0 unknown); within ±100 ms of the played length (2.17) |
| 24 | 8 | qfp | 2.3.5, of the card file (0 unknown; the software MUST fill it, the device MAY) |
| 32 | 4 | known | which fields the producer looked for (table below) |
| 36 | 2 | flags | table below |
| 38 | 2 | year | (0) |
| 40 | 2 | track | track number (0) |
| 42 | 2 | trackTotal | (0) |
| 44 | 2 | disc | disc number (0) |
| 46 | 2 | discTotal | (0) |
| 48 | 2 | bpm10 | BPM × 10 (0) |
| 50 | 2 | rgTrackGain | i16, ReplayGain track gain in hundredths of a dB (valid with HAS_RG_TRACK) |
| 52 | 2 | rgAlbumGain | i16, album gain, the same (valid with HAS_RG_ALBUM) |
| 54 | 2 | rgTrackPeak | peak × 10,000, saturating at 65,535 (0 none) |
| 56 | 2 | rgAlbumPeak | the same |
| 58 | 1 | container | 0 unknown, 1 MP3, 2 FLAC, 3 Opus, 4-254 reserved, 255 not audio (an owned image or other file) |
| 59 | 1 | camelot | 0 none; 1-12 = 1A-12A (minor); 13-24 = 1B-12B (major) |
| 60 | 4 | picOffset | file offset of the elected embedded picture's anchor, per picCoding (below; 0 none) |
| 64 | 4 | picLength | its stored length, per picCoding (below) |
| 68 | 1 | picType | ID3/FLAC picture type (3 = front cover); meaningful only with picOffset |
| 69 | 1 | picMime | 0 none, 1 JPEG, 2 PNG, 3 other |
| 70 | 1 | picCoding | 0 raw, 1 ID3 unsynchronised, 2 base64 FLAC PICTURE across Ogg pages, 3 APEv2 binary item |
| 71 | 1 | reserved | 0 |

**The path** of a record, relative to `/music`, is its folder chain's
names and its own name joined with `/`. Its path hash (2.3.3) is over
`/music/` + that path.

**`flags` (u16):**

| Bits | Meaning |
|---|---|
| 0-1 | compilation: 0 not said, 1 yes, 2 said no |
| 2 | HAS_RG_TRACK |
| 3 | HAS_RG_ALBUM |
| 4 | RG_FROM_R128: an Opus R128 gain q (a Q7.8 integer), converted: `rg = (q / 256 + 5) × 100`, computed in integers as `(q + 1280) × 25 / 64` rounded half away from zero (the reference moves from −23 LUFS to −18 LUFS; 5.3) |
| 5 | NO_TAGS: the file was read and carries no tags |
| 6 | UNREADABLE: the producer couldn't parse it; every tag field is absent |
| 7 | TRUNCATED: a value or a list was cut (2.3.6) |
| 8 | BPM_ANALYSED: bpm10 and camelot are mStream's analysis, not the file's tags |
| 9 | FROM_API: the tag fields are mStream's API values, not a reading of the card file (2.7) |
| 10-15 | reserved |

**The picture's anchor** (picOffset, picLength), per picCoding:

| picCoding | picOffset | picLength | The reader |
|---|---|---|---|
| 0 raw (ID3v2 APIC, FLAC PICTURE) | the image data's first byte, after the frame's or block's own header | the image data's bytes | reads them as they are |
| 1 ID3 unsynchronised (v2.3 tag-level, v2.4 tag- or frame-level) | the image data's first stored byte | the stored bytes, before re-synchronising | undoes the unsynchronisation as it reads |
| 2 base64 across Ogg pages (Opus `METADATA_BLOCK_PICTURE`) | the value's first base64 character, after the `=` | the number of base64 characters; the Ogg page headers in between are not counted | skips each page header it meets, decodes, and takes the image data from the decoded FLAC PICTURE block |
| 3 APEv2 binary item | the first byte after the description's NUL | the item's remaining bytes | reads them as they are |

A compressed or encrypted ID3v2 frame (the v2.3 and v2.4 frame flags) is
never elected: the election (5.3) passes over it to the next picture.

**`known` (u32)**, one bit per field the producer looked for. A known
field that is zero or empty is *absent from the file*; an unknown one is
*not reported*. The builder treats both the same way (2.9); the bits are
for diagnostics, for the parity test, and for Stage B. So that two
readers of one file agree, `known` doesn't depend on which tags a file
happens to carry:

- a readable audio file, NO_TAGS included: every bit the record's
  readRules define (readRules 1: bits 0-16), whatever its container;
- an UNREADABLE file: 0 (and durationMs 0);
- a FROM_API record: the bits of the fields it filled (2.7);
- a non-audio record (container 255): 0.

A later readRules that reads a new field adds its bit; a record without
it tells the builder that field was never looked for.

| Bit | Field(s) | Bit | Field(s) |
|---|---|---|---|
| 0 | title | 9 | track, trackTotal |
| 1 | artist | 10 | disc, discTotal |
| 2 | album | 11 | durationMs |
| 3 | albumArtist | 12 | bpm10 |
| 4 | genre | 13 | camelot |
| 5 | composer | 14 | ReplayGain (gains and peaks) |
| 6 | the four sort fields | 15 | compilation |
| 7 | the two MusicBrainz ids | 16 | picture |
| 8 | year | 17-31 | reserved |

**Records for non-audio files** (container 255) have strings = 0, known =
0 and qfp filled. They exist so the software can own a file it wrote,
such as a `cover.jpg` (part 7, U9). The device takes nothing from them
but their ownership (2.14.3).

#### 2.6.5 The string run

At `strings`, inside STRS: one byte **n** (the number of fields, 0-255),
then n NUL-terminated strings in this order. A field at an index ≥ n is
absent. Offset 0 is an empty run (no tags).

| # | Field | Holds (part 5 has the reading rule) |
|---|---|---|
| 0 | title | the first value |
| 1 | artist | every artist value, a list (`A feat. B` stays one value) |
| 2 | album | the first value |
| 3 | albumArtist | every album-artist value, a list |
| 4 | genre | every genre item, a list, ID3 numeric genres resolved to names |
| 5 | composer | every composer value, a list |
| 6 | titleSort | the first value |
| 7 | artistSort | the first value |
| 8 | albumSort | the first value |
| 9 | albumArtistSort | the first value |
| 10 | mbAlbumId | the MusicBrainz release id, trimmed |
| 11 | mbRecordingId | the MusicBrainz recording id, trimmed |

- **n** is the index of the last non-empty field plus one; a record with
  no field has strings = 0. So the same values always give the same run.
- A minor version adds fields only at the end of the run; an older reader
  stops at the fields it knows.
- Derived values (mStream's primary artist, the display join, the genre
  split) are **not** stored: they are the builder's (section 5.4).

#### 2.6.6 `ORIG` and `OSTR`: the ledger (80 bytes per row)

Row i describes the same file as record i; the count MUST equal
recordCount, or the section is invalid. The device ignores both
sections. To the software, a source 2 or 3 tags file without a valid ORIG
is a card with no ledger (2.11); the device still uses its records.

| Off | Size | Field | Meaning (absent value) |
|---|---|---|---|
| 0 | 4 | serverPath | OSTR: mStream's `filepath` (`vpath/relative/path.ext`) as the server returned it ("" for an owned file with no server track) |
| 4 | 4 | mstreamId | mStream's track `id` (0; ids ≥ 2^32 are stored as 0) |
| 8 | 4 | albumId | mStream's `album-id` (0) |
| 12 | 4 | artistId | mStream's `artist-id` (0) |
| 16 | 16 | audioHash | mStream's `audio-hash` as 16 bytes (zeros) |
| 32 | 16 | fileHash | mStream's `hash` as 16 bytes (zeros) |
| 48 | 8 | serverModified | mStream's `modified`, epoch ms (0) |
| 56 | 8 | serverSize | mStream's `file-size` of the source (0) |
| 64 | 4 | createdAt | mStream's `created-at` (SQLite `YYYY-MM-DD HH:MM:SS`, UTC) as Unix seconds (0) |
| 68 | 2 | hashV | mStream's `hash-v` (0) |
| 70 | 1 | originFlags | bit 0 HASH_SAMPLED: the server's hashes are sampled digests, exactly when `hash-v` is 2 or more and `file-size` is 26,214,400 bytes (25 MiB) or more (mStream's rust-parser, `file_size >= sample_threshold`); bit 1 VERIFIED: the download's MD5 matched `hash`; bit 2 ADOPTED: the file was on the card before and was taken into the ledger |
| 71 | 1 | convertedTo | 0: the card file is the server's bytes; else the container code (2.6.4) of a conversion |
| 72 | 2 | convertKbps | the conversion's bitrate in kbit/s (0) |
| 74 | 6 | reserved | 0 |

mStream's canonical key is `audio-hash`, else `hash`
(`COALESCE(audio_hash, file_hash)`): the key of AutoDJ rows, stars and
play counts.

#### 2.6.7 `HIDX`: the path-hash index (12 bytes)

`pathHash` u64, then `record` u32, sorted by (pathHash as an unsigned
integer, record). Writers MUST include it when the file has records
(2.6.2); readers MAY build their own instead. A hit is a candidate only:
the reader MUST compare the record's full path before using it.

#### 2.6.8 Order (deterministic output)

Writers MUST order:

- **FOLD** in pre-order from folder 0, siblings by their names' bytes
  (`memcmp`, shorter first on a common prefix);
- **RECS** by folder index, then by name bytes;
- **HIDX** by (hash as an unsigned integer, record);
- **STRS**: the leading NUL, the header's `producer`, the folder names in
  folder order (folder 0's is offset 0, not written), then for each
  record in record order its name, then its run;
- **OSTR**: the leading NUL, then each row's serverPath in row order.

So the canonical order of two files compares their folders' pre-order
positions first (an ancestor before its descendants, siblings by name
bytes), then their names. It is the order of a walk that, in each folder,
lists its files by name and then descends into its subfolders by name.
The same files and values give the same bytes from both producers, and
the conformance tests compare whole files (2.17). Note that this is not
the byte order of the whole path strings: `A` and its subfolders come
before `A B`, so `A/x.mp3` comes before `A B/y.mp3` although the path
strings sort the other way (`41 2F` against `41 20`). A file in
path-string order fails 2.4.3's checks and is absent.

### 2.7 Filling a record: the card file first, mStream's API for the rest

**The rule (PROPOSED):** the software fills a record's tag fields by
reading the file it wrote, as it is on the card, by part 5's reading
rules: lofty 0.25 (MIT/Apache) with mStream's rust-parser selection rules
ported. The record is then by definition what mStream's default scanner
reads from that file, and the device's scanner, following the same
rules, reads the same. "A transfer record and a device record of one
file are equal" becomes a host test (2.17), not a hope. A converted file
is read after conversion, so its record describes the bytes on the card.

**Why not the sync manifest's metadata** (READ, mStream 926b97b1):

- Its `artist` is the display name of the track's primary artist row, and
  that name is the most common spelling across the server's whole library
  (`src/db/artist-aggregate.js`), not this file's tag.
- Its `album` is the album row's name: for albums keyed by a MusicBrainz
  id, the most common `tag_album` among their tracks
  (`src/db/album-aggregate.js`).
- In the manifest, `artists` is always `[artist]`: `manifestPage` doesn't
  run the credits enrichment (`src/api/sync.js`).
- It has no album artist, compilation flag, sort names, MusicBrainz
  release id or album gain.
- `replaygain-track` is written `|| null`, so 0.00 dB arrives as null
  (`src/api/db.js`, `renderMetadataObj`).

A device scan of the same file could never reproduce those values, and
the two modes would disagree.

**What the API supplies** (the server alone knows it):

| Record field | From | Conversion |
|---|---|---|
| bpm10, camelot | the manifest's `bpm`, `musical-key`, only when the card file's tags have none | `bpm10 = round(bpm × 10)`; the key through the Camelot rule (5.3); flags BPM_ANALYSED |
| container | the card file's bytes (an Ogg stream whose first packet is `OpusHead` is Opus, whatever `format` says) | MP3 1, FLAC 2, Opus 3 |
| the ledger (ORIG) | the manifest entry | 2.6.6; hex digests → 16 bytes |
| size, fatTime, qfp | the card, read back | 2.3.4, 2.3.5 |
| FOLD THUMB, the thumbnail | the folder's image, else `album-art` | 2.14 |

**The fallback (FROM_API).** A software with no reader yet, or a file its
reader can't parse, MAY fill the tag fields from the API instead, and MUST
then set FROM_API and only the `known` bits of the fields it filled. A
file its reader can't parse and that it doesn't fill this way gets an
UNREADABLE record without FROM_API: that record settles ownership and
identity only, and the device reads the file itself (2.9, rule 1), since
its parser may succeed where the software's failed.

The fallback's fields:

| Record field | From | Conversion |
|---|---|---|
| title | `metadata.title` | as is |
| artist | `metadata['artist-display']` | one value: mStream has joined a multi-valued tag with ", " |
| album | `metadata.album` | as is (a server-wide spelling, see above) |
| genre | `metadata.genres[]` | joined with U+001F, at most 16 |
| year, track, disc | `year`, `track`, `disk` | null → 0 |
| trackTotal, discTotal, composer, mbRecordingId | the full set (`/api/v1/db/metadata/batch`): `track-total`, `disc-total`, `composer`, `musicbrainz-recording-id` | |
| durationMs | `duration` (seconds, real) | round(× 1000) |
| rgTrackGain | `replaygain-track` (dB) | round(× 100), HAS_RG_TRACK; null → absent |
| albumArtist, compilation, sort fields, mbAlbumId, album gain, peaks, picture | not in the API | not known |

The device uses a FROM_API record like any other (one record, whole). The
flag exists so the parity test skips it and the console can say where a
name came from.

### 2.8 Paths

#### 2.8.1 The music root

- The library is `/music` at the volume root. FAT finds it whatever its
  stored case; path hashes always use the literal lowercase `/music`.
- The software creates it if missing, and never deletes it.

#### 2.8.2 What the device can see (firmware 0.7.0; `device.txt` may widen it)

- Audio files are `.mp3`, `.flac` and `.opus`, by extension, in any case
  (`LibraryIndex::addFile`). An `.ogg` or `.oga` file is never listed,
  whatever its codec; `device.txt`'s `extensions` key says which
  extensions a firmware lists (2.15).
- Cover images are `.jpg` and `.jpeg` (`LibraryIndex::imageRank`).
- A name starting with `.` is skipped, file or folder.
- The path from the volume root (`/music/…`) is at most **255 bytes of
  UTF-8**; longer ones are skipped.
- At most **8 folder levels** below `/music` (`Library.cpp`, `kMaxDepth`).
- Each name is at most 255 UTF-16 code units, FAT's own limit.

#### 2.8.3 From a server path to a card path (the software)

The result is recorded, so the ledger, not this algorithm, is
authoritative for existing files. The algorithm is RECOMMENDED so the
terminal and a future device sync name new files alike.

1. **Split** mStream's `filepath` at `/` into [vpath, c1, …, cn]. By
   default the vpath is dropped and the card path is `c1/…/cn`; under the
   vpath layout (2.8.6) it is kept as the first folder.
2. **Each component:**
   1. convert to NFC;
   2. replace each of U+0000-U+001F, U+007F and `" * / : < > ? \ |`
      with `_`;
   3. strip leading spaces, and trailing spaces and dots (FAT and Windows
      drop trailing ones silently);
   4. replace leading dots with `_`, or the device would hide the name;
   5. if the result is empty, use `_`;
   6. for the Windows device names `CON PRN AUX NUL COM1-COM9 LPT1-LPT9`
      (any case, with or without an extension), append `_` to the stem.
3. **The extension** stays as the server has it when it is one of
   `device.txt`'s `extensions` (2.15), in any case. A converted file
   takes its new one (`.opus`, `.mp3`). An Opus stream the server stores
   as `.ogg` or `.oga` (mStream's `format` is the extension, so it says
   `ogg` or `oga`) is copied byte for byte and named `.opus`: no
   transcode, `convertedTo` 0. Any other extension MUST NOT reach the
   card for an audio file.
4. **Depth.** Past 8 folder levels, join the 8th and deeper folders into
   one, with ` - ` between them.
5. **Length.** While `/music/` + the path exceeds 255 bytes, or a name
   exceeds 255 UTF-16 units:
   1. take the longest component (ties: the deepest; for the file, its
      stem);
   2. cut it at a code-point boundary;
   3. append `~` and 4 uppercase hex digits: the low 16 bits of the
      FNV-1a 64 of the original component's bytes.
6. **Collisions.** Within a folder, compare names by their **match key**:
   the NFC form, then each code point's Unicode simple upper case. That
   is FAT's case-insensitive rule, and a little wider (FAT itself keeps
   an NFC and an NFD spelling apart; the key merges them, so no
   look-alike twin is ever made). An existing folder's on-card spelling
   wins, so the folders merge. Of two
   files that collide, the one with the lower mStream id keeps the name;
   the others take ` (2)`, ` (3)`… before the extension. An "already
   exists" from the OS is also a collision.
7. **Read back** the names after writing, brought back to the stored
   form by 2.8.5's per-OS rule, and record those.

About 0.6% of the paths in a measured real library need these rules:
illegal characters, a trailing dot or space, case collisions (MEASURED,
metascan section 5.7).

#### 2.8.4 Limits to respect

- A FAT32 file is at most 4 GiB − 1 bytes. The software MUST skip (and
  report) a larger one, or convert it.
- A FAT32 folder holds at most 65,536 directory entries; a file uses
  1 + ⌈UTF-16 length / 13⌉ of them.
- The device searches folders linearly, so the software SHOULD keep
  folders under about 2,000 entries.

#### 2.8.5 Names as stored

The card is the reference. **A name's canonical spelling is what FatFs
returns on the device:** the stored UTF-16 long name converted to UTF-8
(UTF-8 API, long names, code page 850 for short names). Every path hash
and every name in a record is that spelling. Not every PC lists it
unchanged, so the software brings its listing back to it:

- **Windows** lists the stored long name unchanged. Nothing to do.
- **macOS** stores names on FAT precomposed (NFC) but returns them
  decomposed (NFD) from `readdir()`, as it does for HFS+ (the reason for
  Git's `core.precomposeUnicode`). The software MUST convert every
  listed name to NFC. That is exact for every name the software creates
  (NFC, 2.8.3) and every name macOS or Windows wrote. A name some other
  system stored in NFD reads wrong on a Mac: the device then doesn't
  find T's record for it and scans the file itself (slower, never wrong),
  and the match key (2.10.2) still pairs it with its ledger row.
- **Linux:** a vfat mount without the `utf8` option or `iocharset=utf8`
  lists `?` or `:xxxx` escapes for names outside its character set. The
  software MUST read the mount's options (`/proc/self/mountinfo`) and
  refuse such a mount, saying how to remount it.
- **Others:** the software MUST refuse to write unless a C1-style check
  (6.3) has shown that the platform lists stored names unchanged.

Names that exist only as 8.3 short names with bytes above 0x7F may read
differently on the device's code page and a PC's; only hand-made files
have such names, and the device scans those itself.

#### 2.8.6 vpaths and library roots

The device names a folder album by its depth below a root: the first
folder is the artist, the second the album.

- **The default layout** drops the vpath: the card mirrors each library's
  own tree, and `/music` is the only root. Folders of two libraries with
  the same name merge.
- **The vpath layout** keeps a `/music/<vpath>/` folder per library. The
  software then MUST list each `<vpath>` in the root's `LIBR` section.
- **On the device,** a file's root is the longest LIBR folder that
  contains it, else `/music`. That applies to the listener's own files
  too.

Which layout is part 7's U2.

### 2.9 Precedence and merge (the device's builder)

One builder makes `library.idx`. Its inputs:

- **W**, the walk's listing: path, size and fatTime of every file;
- **T**, the valid MPTG companion of the valid root (2.5), if any;
- **D**, `/.player/tags.bin`, if valid;
- the file names.

For each audio file the walk found, take **one** record, whole:

1. **T's record** for that path, if its size equals the file's and its
   fatTime matches (equal, or equal after T's skew, 2.3.4), or its qfp
   was confirmed against the file (the device computes qfp only for a
   size match whose time doesn't match, and saves the confirmation, so
   it is paid once per file). Source: transfer. **Except** a T record
   flagged UNREADABLE without FROM_API: it settles only that the file is
   the software's and unchanged; for its names the builder goes on to
   rule 2, and the scan may read the file (3.3.1).
2. Otherwise **D's record** for that path, if it is a full record (the
   device's status rows for files T covers don't count, 3.3.2), its size
   and fatTime are equal, and D's parserVersion is one the builder still
   accepts (records of an older parser are rescanned). Source: device.
3. Otherwise **no record:** the file is named from its path, as today, and
   goes on the scan's list.

**Field by field,** a field absent or not known in the chosen record falls
back to the **path parse** only (today's `trackname` reading): the title
from the file name, the number and disc from the file name, the album from
the folder, the artist from the artist folder. It never falls back to the
other producer's record. A file with no record indexes exactly as today.

**Further rules:**

- **Before the first walk after a new commit,** the builder MAY take T's
  listing as W: the software listed the card moments ago. D's Software
  rows (3.3.2) whose paths the new T no longer lists are then dropped:
  the commit deleted those files, or gave them up to the listener, and
  the walk re-adds any still on the card. The device's background walk
  then confirms, and a mismatch makes the file's record fall to rule 2
  or 3 at the next build (section 3.2).
- Records for paths the walk didn't find are ignored. The device MAY drop
  them from D at its next rewrite; it never touches T.
- The device's scan never reads a file rule 1 covers (an UNREADABLE T
  record aside): transfer records save the scan.
- Paths match by their exact bytes on the device. A folder the listener
  renamed by case alone (`ACME` to `Acme`) misses T until the software's
  next run takes the new spelling (2.10.2); its files are scanned
  meanwhile.
- **Rescan tags** on the device rewrites D's records only; rule 1 still
  prefers a matching T record (part 7, U8: decided so).
- Covers have their own order (2.14.3).
- What the builder makes of the chosen fields (the votes, the display
  join, the orders) is part 5.

### 2.10 Change detection

#### 2.10.1 The device

1. **Boot.** Read the root (2.5.4: two small reads). Compare (cardId,
   generation, commitId, T's headerCrc), the builder's rules version and
   the index's version with what `library.idx` recorded at its last build.
   Equal: load the index. Different: build from the records (section
   3.2).
2. **The walk** reads names, sizes and FAT fields from the directory
   itself (`f_readdir`). A per-folder digest over its sorted (name, size,
   fatTime) entries lets an unchanged folder be skipped. A folder's own
   FAT time isn't used: FAT doesn't update it when its contents change.
3. **Per file,** section 2.9's matches. The device computes qfp only to
   confirm a doubtful match, or on a listener's Verify.

#### 2.10.2 The software

1. **The server.**
   - **A 304 means "probably unchanged".** The manifest's `revision` (its
     ETag, sent back as If-None-Match on the first page) is the visible
     tracks' count, highest id, newest `modified` and number with art
     (`manifestRevision`, `src/api/sync.js`). mStream's scanner updates a
     changed file's row in place and keeps its id (an UPSERT), so a file
     replaced by one whose mtime isn't the library's newest (a remaster
     copied with its old mtimes kept) leaves `revision` unchanged. So the
     software reads the whole manifest without If-None-Match at least
     every 7 days or every 10th run (proposed), and in the deep check;
     part 4's A10 would close the gap.
   - **Pairing.** A ledger row pairs with the manifest entry of the same
     serverPath. A row whose serverPath is gone pairs with the one
     unpaired entry of the same audioHash (else fileHash): a move on the
     server, which the software MAY carry out as a rename on the card
     instead of a delete and a download. Several candidates: no pair. A
     row never pairs by `mstreamId` alone: ids change when the server's
     database is rebuilt; the row's ids are refreshed from its entry.
   - A paired row is unchanged when audioHash, serverSize and
     serverModified are, and fileHash too when present. `revision` counts
     the tracks with art, so an art backfill alone gives a new revision:
     then only the thumbnails are redone.
   - While the manifest says `scanning: true`, a missing path MUST NOT be
     read as a deletion.
2. **The card.**
   - **List `/music`** and match each ledger row to the listing **one
     path component at a time**, from `/music` down: a component matches
     the listed entry with the same bytes, else the one listed entry with
     the same match key (2.8.3, step 6: FAT's case-insensitive rule).
     Several entries with that key: no match.
   - **A match under another spelling** (the listener renamed `ACME` to
     `Acme` on a PC) is followed: the ledger, T's names, the path hashes,
     HIDX, DJPH and the thumbnail's name take the listed spelling. FAT
     would resolve the old spelling to the same file, so a byte-exact
     comparison would read every file as missing and the collision rule
     would then write `01 (2).mp3` beside each one.
   - Then compare each matched file's (size, fatTime), applying the skew
     rule (2.3.4) to the previous ledger:
     - equal: unchanged;
     - size equal, time different, no skew: compare qfp. Equal: unchanged
       (record the new fatTime);
     - otherwise: changed by the listener.
   - **Unmatched: missing.** Before planning "write again", the software
     looks among the files no ledger row matched for exactly one with the
     row's size and then its qfp: a move by the listener (a folder renamed
     `Album` to `Album (2019)`, say). Found: it follows the move, the row
     taking the new path (proposed; part 7, U4), rather than copying the
     album a second time. Not found: deleted by the listener.
3. **Carried records.** A record carried from the previous tags file is
   read again from the card file when the software's reader version or
   readRules changed (2.6.1).
4. **The plan:**

| Server | Card | Action |
|---|---|---|
| new in the selection | path free | write |
| changed | unchanged | replace |
| unchanged | unchanged | nothing (re-list its stamp) |
| any | changed by the listener | keep the listener's file, drop it from the ledger, report it (part 7, U5) |
| in the selection | moved by the listener | follow it (part 7, U4) |
| in the selection | missing | write again (part 7, U4) |
| left the selection | unchanged | delete |
| left the selection | changed by the listener | keep, drop from the ledger, report |

5. **A mass-deletion guard.** When the plan would delete more than half of
   the ledger's files, or most of the ledger's audio hashes are missing
   from the whole manifest (another server, a rebuilt or emptied
   library), the software asks before deleting anything (part 7, U7).

An optional **deep check** compares qfp for every owned file: two reads
each, about 1-2 minutes for 20,000 files through a USB reader
(ESTIMATED).

### 2.11 Who may write and delete what

| Path | The software | The device | The listener |
|---|---|---|---|
| `/music/**` files in the committed ledger whose (size, fatTime) still match it (paths matched as 2.10.2 says) | create, replace, delete | read | anything |
| `/music/**` write targets of the current plan (`pending.bin`, or a valid `pending.tmp` when it is missing: 2.12.5) that the ledger doesn't list: the half-finished work of a cut run | replace, delete | read | anything |
| `/music/**` all other files | read and list only (qfp allowed) | read | anything |
| `/music/**` folders | create; remove only OWNED folders that are empty (OWNED in the ledger, or a folder op of the current plan: 2.12.5) | none | anything |
| `/music` itself | create if missing | read | anything |
| `/.mstream/**` | everything | **read only** (except as the sync agent: 2.12.4) | may delete it all (the device then scans; the software then sees a card with no ledger) |
| `/.player/**` | **read only**; it relies on `device.txt` alone | everything | may delete it all (the device rebuilds) |
| anything else on the card | never | never | anything |

- **A file the listener changed** stops being the software's at the next
  run (2.10.2). The software MUST NOT replace or delete it without the
  listener's explicit say-so.
- **Which server filled the card** (2.5.1). The root's server is the
  same as the one the software talks to when both serverInstance values
  are known and equal, or, failing that, when the root's serverUrlKey is
  this URL's key or one the listener has already called this card's
  server. It is **different** when both serverInstance values are known
  and differ, and **unknown** otherwise. Different or unknown: the
  software MUST NOT delete or replace the files that root owns without
  asking (part 7, U7). The guard of 2.10.2, step 5, applies as well.
- **A card with music and no ledger:** every file on it is the
  listener's. The software MAY take files that equal what it would write
  into the ledger (ADOPTED), only with the listener's consent (part 7,
  U6).
- **The device never writes or deletes anything under `/music`.** A
  future "delete from the card" on the device would be a listener's
  action; its effect on the ledger is part 7's U4.

### 2.12 Atomicity and crash safety

**The principle:** no contract file is modified in place. Each is written
aside and renamed into place, or written under a new generation-numbered
name that nothing references until a root names it. Every reader
validates sizes and CRCs, and an invalid file is absent. So a card pulled
at any moment leaves the last committed state, plus files the identity
rules handle.

#### 2.12.1 A transfer run

1. **Settle the root.** Pick it (2.5.4). If `manifest.tmp` and
   `manifest.bin` are one commit seen twice (the same commitId and
   headerCrc), go to step 3's disk check and touch neither. Otherwise, if
   `manifest.tmp` won, finish its rename: delete `manifest.bin`, then
   rename.
2. **Settle the plan.** The cut run's plan is `pending.bin`, or a valid
   `pending.tmp` when `pending.bin` is missing (2.12.5); a `pending.tmp`
   that wins is renamed into place. It is read now, before step 4 deletes
   any `*.tmp`.
3. **Look for cut renames** (below). If one may have happened, the run
   stops before it deletes or writes anything, and asks the listener to
   run the OS's disk check (`chkdsk /f` on Windows, `fsck.vfat` on Linux,
   First Aid on macOS) and then to start it again.
4. **Clean up.** Delete everything in `stage/`, every `*.tmp` in
   `/.mstream` and its `thumbs/` folders, and every companion the root
   doesn't name. If there was a plan, the last run was cut short: recover
   (2.12.3).
5. **List** the card; read the server's selection; plan (2.10.2).
6. **Write `pending.bin`** (MSPD, 2.12.5) through `pending.tmp` and a
   rename: the paths this run will delete, the folders it will create,
   the paths it will write.
7. **Delete first.** Ledger files leaving the card go, only when their
   identity still matches. This frees space on a full card; a cut here
   only leaves fewer files.
8. **Copy each file:**
   1. download into `stage/<n>.tmp` (an 8.3 name; n is the op's index in
      the current PEND, in hex), computing qfp, and the MD5 when the
      server's hash is a full digest;
   2. check the MD5 against `hash` when HASH_SAMPLED is clear and the file
      isn't converted (VERIFIED);
   3. close it and rename it into its `/music` path, creating the plan's
      folders (OWNED) as needed;
   4. read back its name, size and fatTime; read its tags (2.7).
9. **Checkpoint** every 500 files or 5 minutes, between two files:
   1. write `tags-<g>.bin` (g = the root's generation + 1) under its final
      name;
   2. write `manifest.tmp` (FINAL clear), delete `manifest.bin`, rename;
   3. delete the previous tags file;
   4. rewrite `pending.bin` with the rest of the plan, through
      `pending.tmp`;
   5. go on in the next generation.
10. **Thumbnails:** for each album whose art is new or changed, write
    `thumbs/<h>/<8 HEX>.tmp`, then delete the old `.565` and rename. Then,
    at every run, check every THUMB folder's file: it exists, is 21,656
    bytes, and its header's pathHash is the folder's; rewrite any that
    isn't. (Whether the art changed is in the software's private state,
    which a cut can leave ahead of the card.)
11. **AutoDJ:** write `autodj-<g>.bin` when 2.13.4 says so; otherwise the
    new root names the old one again.
12. **The final commit** is step 9's write and rename with FINAL set; then
    delete `pending.bin`.
13. **Collect:** delete the companions the root doesn't name, thumbnails
    whose folder no longer has THUMB, and empty OWNED folders.

**Renames over an existing file:** Rust's `fs::rename` may or may not
replace atomically on a FAT driver (Windows `MoveFileExW` with
REPLACE_EXISTING is not atomic on FAT), so the software deletes the old
file first and then renames. The root rule (2.5.4) makes either outcome
safe for `manifest.bin`. Other files are named so that the old and the
new never share a name, except two: a replaced music file, which the
identity rules catch, and a replaced thumbnail, which step 10's check
catches.

**Cut renames.** A rename on FAT writes the new directory entry and then
removes the old one (FatFs's `f_rename`: `dir_register`, then
`dir_remove`; a PC's driver may do the same). A cut between the two
leaves two entries on one cluster chain. Deleting either one then, or
truncating it, frees clusters the other still uses: the next download
reuses them, and the survivor plays another file's bytes while its size
and time still match. So step 3 looks for the pairs a cut rename leaves:

- a plan's write op n whose `stage/<n>.tmp` and whose target both exist,
  with the same size and the same qfp;
- `manifest.tmp` and `manifest.bin` that are one commit (step 1);
- `pending.tmp` and `pending.bin`, or a thumbnail's `.tmp` and `.565`,
  that are byte-equal.

A legitimate pair can look the same (a replacement with identical bytes);
it costs one unneeded disk check. A disk check copies cross-linked chains
apart (`chkdsk`) or asks what to do (`fsck.vfat`); the identity rules
handle what is left. The device's own renames follow 2.12.6.

#### 2.12.2 A card pulled during a run

| Pulled during | What the card holds | The device | The software's next run |
|---|---|---|---|
| a download | a partial `stage/n.tmp` | never sees it (a hidden folder) | deletes it (step 4) |
| the rename into `/music` | the old file or the new one | identity mismatch: scans the new one itself | replaces it in place (the plan, 2.12.3) |
| the rename into `/music`, between its two directory writes | the stage file and the target on one cluster chain | reads the target (the new bytes): identity mismatch, scans it | stops for a disk check (step 3) |
| the deletions | some files gone | gone files are absent | plans again |
| a tags file's write | a partial `tags-<g>.bin` nothing names | ignored | deletes it |
| `manifest.tmp`'s write | an invalid tmp | uses `manifest.bin` | rewrites it |
| between deleting `manifest.bin` and the rename | a valid tmp only | uses the tmp | finishes the rename |
| the rename of `manifest.tmp`, between its two directory writes | both names, one commit, one chain | uses either | stops for a disk check (step 3) |
| `pending.bin`'s replacement | `pending.tmp` only | says the last transfer didn't finish | reads the tmp as the plan (step 2) |
| a thumbnail's write | a partial `.tmp` | ignored (wrong name; the header is checked too) | deletes it |
| a thumbnail's replacement, after the old `.565` went | THUMB set, no `.565` | its own cover order (2.14.3) | rewrites it (step 10's check) |
| the collection | leftovers | ignored | collects again |

**FAT itself:** an interrupted directory or FAT update can leave lost
clusters, and rarely a cross-linked chain. The software SHOULD ask for
the card to be ejected before it is pulled, and MAY offer the OS's disk
check after a cut.

#### 2.12.3 Recovering a cut-short run

The plan (2.12.5) lists each write target with its expected size, and
each folder the run was to create.

- **A write target** whose identity equals the ledger's (the rename never
  happened, or the commit did): nothing to do. Otherwise it is the
  software's half-finished work, which 2.11's plan row lets the software
  replace or delete: a fresh copy **replaces it in place**, never beside
  it (no ` (2)`), or it is deleted if the new plan doesn't want it.
- **A folder op** whose folder exists and isn't OWNED in the ledger was
  made by the cut run: the next commit records it OWNED, and step 13
  removes it when it is empty.
- A listener's own file or folder placed at exactly that path between the
  two runs is the one case this gets wrong, accepted as too rare to
  matter.

#### 2.12.4 The device as the sync agent (a future WiFi sync)

The device's sync task writes `/.mstream` with source 3, following
2.12.1-2.12.3 exactly: the same cardId, the next generation, ORIG rows
from the API, records read from the bytes it wrote. The terminal and the
device can then take turns on one card. The device MUST NOT write
`/.mstream` in any other role.

#### 2.12.5 `pending.bin` (MSPD)

- **The header** (headerBytes = 56): the common part, then runId (u64,
  random), baseGeneration (u32) and 4 reserved bytes.
- **Sections:** `PEND`, REQUIRED, stride 16, one entry per op: op (u8: 1
  write, 2 delete, 3 folder: a folder the run will create), flags (u8,
  0), 2 reserved bytes, path (u32, STRS, relative to `/music`),
  expectedSize (u32; 0 for ops 2 and 3), reserved (u32); and `STRS`.
- **Order:** the deletes, then the folders, then the writes, each group
  in canonical order (2.6.8; folders in pre-order).
- **Which file is the plan:** `pending.bin`; when it is missing, a valid
  `pending.tmp`; when both are valid, the one with the higher header
  generation, else `pending.bin` (2.5.4's rule). It is replaced through
  `pending.tmp`: delete, then rename.
- **The device** reads only the plan's presence ("the last transfer
  didn't finish") and MAY say so on the Library tab's status line.

#### 2.12.6 The device's own files (device-internal; for completeness)

- **The scan** appends finished records to `/.player/tags.jnl` every ~100
  files or ~5 s, each chunk with its own CRC; a reboot replays it up to
  the first bad chunk. A compaction writes `tags.tmp`, deletes
  `tags.bin`, renames, then deletes the journal (section 3.3).
- **`library.idx`, `queue.txt`, the thumbnails and `opus.idx`** keep
  today's write-aside-then-rename; when only the tmp is there, it is the
  newest whole file.
- **`device.txt`** is written through `device.tmp` and a rename.
- **A cut inside one of these renames** (2.12.1, "Cut renames") leaves
  `X` and `X.tmp` on one cluster chain, and the device's next write of
  `X.tmp` (`FA_CREATE_ALWAYS` truncates) would free clusters `X` still
  uses. So when both exist, the device compares their first clusters
  (`FIL.obj.sclust` after `f_open`):
  - different: the tmp is a leftover, deleted as today;
  - equal: the tmp is renamed `X.xl1` (a rename frees nothing); the next
    replacement of `X` renames it to `X.xl2` instead of removing it; the
    device never deletes an `.xl1` or `.xl2` file while it shares its
    first cluster with its twin, and deletes both once they don't (a
    disk check copies cross-linked chains apart). The console and the
    Library row say the card wants a disk check on a PC.
  - The window is two sector writes, and the `LibraryWrite` blocker
    (3.3.5) keeps the power on through it; a brownout or a crash is what
    remains.

### 2.13 AutoDJ: `autodj-<gen>.bin` (MPDJ v1)

A precomputed top-K neighbour table over the card's tracks, built by the
software from mStream's embeddings (pulled through
`/api/v1/discovery/local/embeddings`, or later a bulk route: part 4, A2).
On the device a pick reads one row of K neighbours (300 B at K = 100) and
merges the last few picks' rows (autodj research, sections 2 and 7).

#### 2.13.1 Header (type-specific part; headerBytes = 96)

| Off | Size | Field | Meaning |
|---|---|---|---|
| 40 | 4 | rowCount | rows (one per distinct embedded recording on the card) |
| 44 | 4 | pathCount | DJPH entries |
| 48 | 2 | k | neighbours per row (100 recommended) |
| 50 | 1 | indexBytes | 2 when rowCount ≤ 65,535, else 4 |
| 51 | 1 | scoreKind | 1: u8 = clamp(round(cosine × 255), 0, 255), computed as 2.13.2 says; any other value: the file is absent |
| 52 | 4 | builtTime | Unix seconds; informational |
| 56 | 16 | selectionSig | 2.13.3, the digest's first 16 bytes in order (2.3.1) |
| 72 | 4 | modelId | STRS: the embedding model's id, as mStream reports it |
| 76 | 4 | modelVersion | STRS |
| 80 | 4 | metric | STRS: `cosine` |
| 84 | 4 | tagsGeneration | the MPTG companion's generation the rows were built against (diagnostic) |
| 88 | 4 | license | STRS: the licence of the data the table derives from, as mStream's model block gives it (`CC-BY-NC-SA-4.0` today) |
| 92 | 4 | attribution | STRS: the model's attribution string |

#### 2.13.2 Sections

| Type | Flags | Stride | Content |
|---|---|---|---|
| `DJRW` | REQUIRED | 24 | rows |
| `DJNB` | REQUIRED | k × (indexBytes + 1) | each row's neighbours |
| `DJPH` | REQUIRED | 12 | card files → rows |
| `STRS` | REQUIRED | blob | strings |

**`DJRW`** (24 bytes):

| Off | Size | Field |
|---|---|---|
| 0 | 8 | pathHash: the smallest path hash, as an unsigned integer, among the row's card files (its primary file) |
| 8 | 8 | hashPrefix: the first 8 bytes of the canonical hash (`audio-hash`, else `hash`), as bytes in their hex order (2.3.1) |
| 16 | 2 | bpm10, as in RECS (the file's tag, else the server's analysis) |
| 18 | 1 | camelot, as in RECS |
| 19 | 1 | flags: bit 0 BPM_ANALYSED |
| 20 | 4 | artistKey: the low 32 bits of the FNV-1a 64 of nameKey (5.4) of the primary file's first artist value; 0 none |

**Row order:** DJRW is sorted by the canonical hash, ascending, as 16
bytes (`memcmp`): the order of 2.13.3's hash lines. A row's index is its
position in that order.

**`DJNB`:** row r's list starts at r × stride.

- Each entry is a row index (indexBytes, little-endian), then a score byte.
- Entries are sorted by score descending, then index ascending.
- Unused slots have an index of all ones and a score of 0.
- A list never holds its own row, nor a row of the same song: equal
  `nameKey(artist display) + "|" + nameKey(title)` (5.4) of the rows'
  primary files.

**The scores, exactly** (so two writers pick the same neighbours):

1. The cosine of rows a and b is the dot product of their embeddings
   (mStream L2-normalises them; nothing is normalised again): the sum,
   in index order 0 to dim − 1, of the products of the f32 components,
   each product and each partial sum in f64. A product of two f32 is
   exact in f64, so the result doesn't depend on a fused multiply-add.
2. The score is `clamp(round(cosine × 255), 0, 255)`, computed in f64
   and rounded half away from zero.
3. **Quantise first, then select:** row a's list is the K best other rows
   by (score descending, row index ascending), leaving out a's own row
   and the rows of the same song. Fewer than K: unused slots.

**`DJPH`:** `pathHash` u64, then `row` u32, sorted by (pathHash as an
unsigned integer, row). Every card file that has a row appears;
duplicates (one recording at several paths) share one row.

#### 2.13.3 The selection signature

The first 16 bytes of the SHA-256 of this text, every line (the last
included) ended by one LF:

```
MPDJ-SEL 1
model <modelId> <modelVersion>
metric <metric>
k <k>
<canonical hash>        one line per row: 32 lowercase hex digits, sorted ascending, no duplicates
```

The canonical hash is `audio-hash`, else `hash`, the key mStream files
embeddings under. Only tracks with an embedding have rows, so the
signature changes whenever the set of rows would: the selection changes,
a track gets its embedding, or the model, the metric or K change.

**Test vector:** model `discogs-effnet` version `1`, metric `cosine`,
k 100, and the hashes `0123456789abcdef0123456789abcdef` and
`fedcba9876543210fedcba9876543210` give
**9f0ae91f220e2e76ec5a40e49fc80b2d**.

#### 2.13.4 Its tie to the root, and the gaps

- The root's `COMP` entry for the table carries its selection signature
  as `key`; the table is valid only when the root names it and the keys
  agree (2.5.3).
- **The software** writes a new table when the signature it computes
  differs from the root's key (a new neighbour pass), or when DJPH would
  change (a recording's card path changed: the rows and neighbours are
  reused, the map rewritten). Otherwise each new root names the old file
  again. The device skips neighbours whose path hash isn't on the card.
- **The device** never computes the signature; it compares bytes.
- **Gaps:** the listener's own files, and tracks not yet embedded, have
  no row. With one of those as the anchor, AutoDJ uses **random with
  filters**, narrowed in v1 to the data that is resident:
  - the anchor's BPM and key come from its chosen record (one read of
    its record at pick time);
  - candidates are filtered by the BPM and key of their DJRW rows, the
    only per-track filter data in PSRAM (3.5); candidates without a row
    are eligible only when the anchor has neither BPM nor key;
  - no genre filter: genres aren't resident (no Genres view yet);
  - a card with no MPDJ at all gets plain random.

  A per-track filter block in `library.idx` (bpm10 and camelot, about
  4 B per track, 80 KB at 20k) would widen this to every file; it is a
  lever for part 7, U15, not in the 3.5 budget.
- **Licence.** The embeddings come from a CC BY-NC-SA 4.0 model
  (mStream's `discovery-features-lib.js`), so the table is derived data
  under that licence. Personal use on the device is fine; no table goes
  into the firmware, a release, the test fixtures or a card made for
  others. mStream's rule that discovery is never gated by or bundled into
  a paid offering holds here too.

### 2.14 Covers

#### 2.14.1 Transfer thumbnails: `/.mstream/thumbs/<h>/<8 HEX>.565`

- **Format:** the device's existing **MPTH v1** (`thumbfile` in
  `lib/core/ThumbCache.h`), byte for byte, so the device reads them with
  today's code.
- **Key:** the **album folder's** path hash (2.3.3), for example of
  `/music/Artist/Album` (the device's own thumbnails key on the cover
  file's path; a transfer thumbnail needs no cover file on the card).
- **The album folder** is the device's (`LibraryIndex`'s `Album.folder`):
  the folder at depth 2 below the file's root (the longest LIBR root
  that contains it, else `/music`: 2.8.6); a file at depth 0 or 1 has
  its own folder. Deeper folders belong to that album, so
  `/music/Artist/Album/CD1/01.flac` takes `/music/Artist/Album`
  (B1F7E69FBD466B59), not `.../CD1` (D9FA96D903A5A10C), and a loose
  `/music/Artist/x.mp3` takes `/music/Artist` (4296E8541CC39B71).
- **A clash of the 32-bit name:** when two album folders' hashes share
  their upper 32 bits, only the folder with the smaller full hash gets a
  thumbnail and THUMB; the other falls to the device's own cover order.
- **Name:** the hash's upper 32 bits as 8 uppercase hex digits, in the
  folder named by its first hex digit: `/music/Artist/Album` hashes to
  B1F7E69FBD466B59, so its file is `thumbs/B/B1F7E69F.565`. 8.3 names,
  so no long-name entries slow the folder.

| Off | Size | Field |
|---|---|---|
| 0 | 4 | magic `MPTH` (the u32 0x4854504D) |
| 4 | 2 | version 1 |
| 6 | 1 | flags: 0 (bit 0, "no picture", is the device's own marker; the software never writes it: no art, no file) |
| 7 | 1 | 0 |
| 8 | 8 | pathHash: the folder's full FNV-1a 64 (a reader checks it, so a clash of the 32-bit name reads as a miss) |
| 16 | 4 | sourceBytes: the size of the image it was made from (informational) |
| 20 | 4 | 40 \| 96 << 16 (the two sizes; a reader checks it) |
| 24 | 3,200 | 40 × 40 pixels |
| 3,224 | 18,432 | 96 × 96 pixels |

- The file is **21,656 bytes**.
- **Pixels:** big-endian RGB565, `(R >> 3) << 11 | (G >> 2) << 5 | (B >> 3)`,
  high byte first; rows top to bottom; **centre crop** (scale until the
  shorter side fills, crop the longer side evenly); an area-averaging
  (box) downscale. mStream's bundled ffmpeg makes the same slots with
  `scale=96:96:force_original_aspect_ratio=increase,crop=96:96 -pix_fmt rgb565be`
  (MEASURED, metascan section 5.6); the terminal's `image` crate can too.
- **The software sets THUMB** on the album folder in FOLD for each
  thumbnail it writes, checks every THUMB folder's file at every run
  (2.12.1, step 10), and removes thumbnails whose folder lost the flag
  (step 13).

#### 2.14.2 The source image: folder first, as the device would choose

So that a transfer thumbnail shows what the device itself would have
shown, the software elects the source by the user's folder-first rule,
looking where `LibraryIndex::albumCover()` looks: the album folder
(2.14.1), else the folder of the album's first track in canonical order
(2.6.8: for disc subfolders, `CD1`):

1. **That server folder's image**, ranked as the device ranks them:
   `cover`, then `folder`, then `front` (`.jpg` or `.jpeg`, any case),
   then the largest other `.jpg`. PNG and progressive JPEG are fine here:
   the PC decodes them. Today the folder can't be listed through the API,
   so the software probes the named files with `HEAD /media/<vpath>/<dir>/<name>`;
   a listing (part 4, A4) makes the ranking exact.
2. **Else** the most common `album-art` among the album's tracks,
   fetched with `GET /album-art/<file>` (the full image, not the `zl-`
   and `zs-` copies). That is mStream's choice: embedded art first by
   default (`albumArtPriority: 'metadata'`), or an online lookup.
3. **Else** no thumbnail.

#### 2.14.3 The device's cover order for an album

1. **The transfer thumbnail**, when the album folder (2.14.1) has THUMB
   in a valid T, the file reads back as MPTH v1 with the folder's hash,
   and neither the album folder nor its first track's folder holds **a
   cover image the ledger doesn't list** (a `.jpg` the listener added by
   hand wins, by step 2).
2. **The folder image** of the album folder, else of its first track's
   folder (`LibraryIndex::albumCover()`), through the device's own
   thumbnail cache (`/.player/thumbs`, keyed by the image's path), else
   decoded: `cover`,
   `folder`, `front`, then the largest other `.jpg` (`LibraryIndex::imageRank`;
   `Thumbs.cpp`'s `pickLargest()` when more than one other `.jpg`). Today's
   behaviour.
3. **The embedded JPEG** (milestone L6): the picture of the first track, in
   album order, whose chosen record has `picOffset` and `picMime` = JPEG;
   read from `picOffset` as its `picCoding` says (2.6.4: raw,
   unsynchronised, base64 across Ogg pages, APEv2), decoded by the
   existing baseline decoder.
4. **The placeholder.**

PNG and progressive JPEG are not decoded on the device; a transfer
thumbnail covers them, since the PC decodes any format.

### 2.15 `/.player/device.txt`: what the firmware reads

The one `/.player` file the software relies on. ASCII, LF line ends, one
`key=value` per line; unknown keys are ignored. The device rewrites it at
boot when its content would change.

```
contract=1
firmware=0.8.0
read.msmf=1
read.mptg=1
read.mpdj=1
read.mpth=1
codecs=mp3,flac,opus
extensions=mp3,flac,opus
max_rate=48000
max_channels=2
```

- `read.*` lists the majors the firmware reads, separated by commas.
- `codecs` lists what it decodes; `extensions` the file extensions its
  walk lists as audio (any case). The device recognises audio by
  extension, not by content, so both matter.
- **The software** writes the newest major of each format that the device
  lists, and converts (or skips) anything not in `codecs`, or above
  `max_rate` or `max_channels`. A card file's extension MUST be one of
  `extensions` (2.8.3, step 3).
- **No file** (a new card, or firmware before this design) means
  `read.*=1`, `codecs=mp3,flac`, `extensions=mp3,flac`, `max_rate=48000`
  and `max_channels=2`. Firmware 0.7.0 plays Opus but can't say so; the
  software MAY ask the listener rather than convert.
- **A key missing** from a file reads as its no-file value; a key with an
  empty value (`read.mpdj=`) says "none". The `contract` line is what
  makes a file a `device.txt`; a reader reads the keys it knows whatever
  the contract number says.

### 2.16 Compatibility summary

| Situation | Outcome |
|---|---|
| The device meets a newer major | That file is absent: no transfer data, the device scans the card itself. Slower, never wrong. `device.txt` lets the software avoid it |
| The device meets a newer minor | It reads the prefix it knows; new sections and fields are skipped |
| The software meets any contract file of a newer major (2.4.5) | Refuses to write; says it needs an update |
| The software meets an older major | Reads it if it still can, and rewrites everything in the newest major the device reads |
| An unknown REQUIRED section | The file is absent |
| An unknown COMP kind | Ignored |
| An unknown flag bit | Ignored |
| An unknown enum value | As 2.4.5's table says for that field |
| Firmware with no `device.txt` | The software writes v1 of everything |
| Damage (any CRC) | That file is absent; a damaged root means no transfer data |
| A new string field | Appended to the run; older readers stop at the fields they know |

### 2.17 Conformance (no device needed)

Both implementations test against shared fixtures, kept in the player
repo in `test/fixtures/card/` (built in N1, under CC0 as proposed so
mstream-terminal and mStream can copy them; part 7, U18) and mirrored
into mstream-terminal. They are `vectors.json`, `libraries/*.json` (the
descriptions), `golden/` (what a writer makes of each) and `hardening/`
(with `index.json`), made by `tools/card_fixtures.py`, an implementation
of this part apart from the player's C++, and frozen: its `--check` fails
on any difference. The folder's README says how to read them.

1. **The vectors** of 2.18, as data files both test suites read.
2. **Writer equality.** A JSON description of a small library (folders,
   files, sizes, stamps, tag values, ledger fields, and every value a
   writer would otherwise choose: ids, times, generations) goes into both
   writers. The C++ writer (host-built, `lib/core`) and the Rust writer
   MUST produce byte-identical MPTG files (2.6.8), and the same for MSMF,
   MPDJ, MSPD and MPTH (an MPTH from given pixels: the scaling filter
   isn't pinned, and PCs' JPEG decoders differ anyway). Tag values go in
   raw: each writer applies 2.3.6 and sets TRUNCATED. MPDJ's fixture uses
   small synthetic embeddings (no real table: 2.13.4's licence), with ties
   at the K boundary and same-song pairs.
3. **Reader parity.** A corpus of synthetic audio files (no real
   library's files) goes through the software's reference reader and the
   device's TagScan built on the host. Their records MUST be field-equal,
   with durationMs within ±100 ms (lofty applies no MP3 encoder-delay
   trim, the device does: about 25-50 ms) and FROM_API and BPM_ANALYSED
   fields excluded. The corpus includes multi-value frames, APE plus
   ID3v1 without ID3v2, ID3v1 filling a blank ID3v2 field, bad UTF-8,
   odd-length UTF-16, ISO-8859-1 bytes 0x80-0x9F, v2.4 tag-level
   unsynchronisation, non-syncsafe v2.4 sizes, a FLAC with a front ID3v2,
   Opus R128 gains, pictures behind large frames, an Opus picture across
   several pages, a compressed and an encrypted APIC frame, and 2.18's
   number strings in every numeric field.
4. **Reader hardening:** truncation at every byte, a flipped bit in every
   section, offsets out of range, strings with no NUL, counts that
   disagree, and every structural check of 2.4.3 broken with valid CRCs
   (a folder that is its own parent, records in path-string order, a
   wrong firstRecord, an unsorted DJPH, an out-of-range neighbour): all
   make the file absent, with no crash and no endless loop. Fuzzed on the
   host. The shared `hardening/` folder has a file for each check of
   2.4.3, so a reader that skips one fails there.
5. **The builder** (host test): a walk listing plus T plus D gives the
   expected `library.idx` checksum; a card filled by the software and the
   same files scanned by the device give the same index (the research's
   M7 test, on the host) apart from each track's source and its length:
   the two producers' lengths may differ by up to 100 ms (item 3), so a
   track's length in whole seconds may differ by one; the skew rule:
   every stamp shifted by +3,600 s still matches, three shifted files
   don't make a skew.
6. **Crash safety** (host, a fake file system that can stop at any
   write, whose rename is two directory writes, new entry first): every
   cut point of 2.12.1 leaves a card the device reads as the old or the
   new commit, the next run brings it to the planned state, and no cut
   ever leads to a cluster chain being freed while an entry still uses
   it (2.12.1's "Cut renames", 2.12.6).

### 2.18 Conformance vectors

Both implementations MUST agree on every vector below; they go into the
fixtures as data (2.17, item 1). The hashes, digests and bytes were
recomputed for this revision with a short Python check; the rule vectors
follow from the sections cited.

**Checksums and hashes** (2.3.2, 2.3.3, 2.5.1):

| Input | Result |
|---|---|
| CRC-32 of ASCII `123456789` | 0xCBF43926 |
| FNV-1a 64 of the empty string, `a`, `/music`, `/music/Artist/Album`, `/music/Artist/Album/01 - Title.mp3`, and `Café` in NFC and NFD | 2.3.3's table |
| FNV-1a 64 of `/music/Artist` | 4296E8541CC39B71 |
| FNV-1a 64 of `/music/Artist/Album/CD1` | D9FA96D903A5A10C |
| A card that stores the folder as `Music` | still hashes as `/music`, 75DC8A6A38687865, never as `/Music`, 359C71D0AA7DCAC5 |
| serverUrlKey of `HTTP://Music.Example:3000/` (normalised: `http://music.example:3000`) | ED901EA3EE763AC7 |

**qfp** (2.3.5), of files whose byte i is i & 0xFF:

| Size | qfp | What it tests |
|---|---|---|
| 0 | A8C7F832281A39C5 | no head, no tail |
| 100 | B708DC48BA0A842D | head only |
| 4,096 | 636A94FE9C19DC15 | head only, full |
| 4,097 | A76BF84EC13A75BC | a 1-byte tail |
| 5,000 | C8E651224ADA889C | a short tail |
| 8,192 | A9383C4532F6F525 | head and tail meet |
| 8,193 | D70E2B23544A7444 | one byte between them, unread |
| 10,000 | F17B194EF7F5F338 | a gap |

**FAT time** (2.3.4):

- 2026-10-07 14:30:42 is 0x5D4773D5; 15:30:42 is 0x5D477BD5; their W
  differ by 3,600.
- 14:30:43 is also 0x5D4773D5: the seconds are halved, rounded down.
- 0x5C0773D5 (month 0) is invalid, like 0: it never matches by time.

**The skew rule** (2.3.4), over pairs whose sizes match:

- 20 pairs, all at Δ +3,600: D = +3,600; all 20 match.
- 7 pairs at +3,600 and no others: no skew (fewer than 8); qfp decides.
- 10 of 30 pairs at +3,600 (the other 20 at 0): no skew (fewer than
  half).
- 20 pairs at +2: no skew (not a multiple of 900).
- 10 pairs at +3,600 and 10 at −3,600: D = −3,600 (equal counts and
  equal |D|: the negative wins); the +3,600 files go to qfp.
- A pair with a recorded or observed 0, or an invalid stamp: left out of
  the pairs, and it never matches by time.

**Bytes** (2.3.1, 2.14.1):

- `MPTG` is the bytes 4D 50 54 47, the u32 0x4754504D; `MPTH` is
  0x4854504D.
- The thumbnail of `/music/Artist/Album` (B1F7E69FBD466B59) is
  `/.mstream/thumbs/B/B1F7E69F.565`, 21,656 bytes; its bytes 0-15 are
  `4D 50 54 48 01 00 00 00 59 6B 46 BD 9F E6 F7 B1` and bytes 20-23
  `28 00 60 00`.
- serverInstance `00112233-4455-6677-8899-aabbccddeeff` is stored as
  `00 11 22 33 44 55 66 77 88 99 AA BB CC DD EE FF`.
- The hashPrefix of the audio hash `0123456789abcdef…` is
  `01 23 45 67 89 AB CD EF`.
- 2.13.3's selection signature is stored as
  `9F 0A E9 1F 22 0E 2E 76 EC 5A 40 E4 9F C8 0B 2D`.

**The selection signature** (2.13.3): 2.13.3's vector gives
9f0ae91f220e2e76ec5a40e49fc80b2d; the same header with no hash lines
gives efdb6ffa02a177c18593a3ed34f7016b. With those two hashes, DJRW row 0
is `0123…` and row 1 `fedc…` (2.13.2's row order).

**Canonical order** (2.6.8, 2.4.3):

- Sibling names `A`, `A B`, `A-`, `B`, `a`, `É` sort as
  A < A B < A- < B < a < É (`41` | `41 20 42` | `41 2D` | `42` | `61` |
  `C3 89`).
- `A/x.mp3` comes before `A B/y.mp3`. A file whose records are in
  path-string order is absent; so is one with a folder that is its own
  parent.

**String runs** (2.3.6, 2.6.5):

- Title `T` only: the run is `01 54 00`.
- Artist `A` only: `02 00 41 00`.
- No field: strings = 0.
- Artist values `X`, `X`, `Y` only: `02 00 58 1F 59 00`.
- `A<TAB>B` is stored as `A B`.
- A 300-byte ASCII title is stored as 255 bytes, TRUNCATED set; 254 ASCII
  bytes and then `é` are cut to 254 bytes, TRUNCATED set.
- Five 204-byte artist values: four kept (819 bytes), TRUNCATED set.
- Values of 255, 255, 255, 250, 10 and 1 bytes: the first four kept
  (1,018 bytes); the 10-byte value ends the list and the 1-byte value is
  dropped with it; TRUNCATED set.

**Numbers** (5.3):

- ReplayGain `-6.785 dB` is −679; `1.005 dB` is 101 (exact decimal: f64
  arithmetic gives 100.49999… and 100, which the rule forbids); `-6.5 DB`
  is −650; `-400 dB`, `inf` and `1e2` are absent.
- R128 gains: −1312 gives −13; 32 gives 513; 0 gives 500; −5888 gives
  −1800.
- Peaks: `0.988567` is 9886; `7` saturates at 65,535.
- BPM: `120.5` gives bpm10 1210; `19.5` gives 200; `300.5`, `0x78` and
  `120 BPM` are absent.

**HASH_SAMPLED** (2.6.6): 26,214,399 bytes with hash-v 2 is a full MD5
(a download can be VERIFIED); 26,214,400 bytes with hash-v 2 is sampled;
30 MB with hash-v 1 is a full MD5.

**The root election** (2.5.4, 2.4.5):

- `manifest.bin` generation 5, `manifest.tmp` 6, both valid: the tmp.
- 5 and 5: the bin. If the two are one commit (the same commitId and
  headerCrc), readers use either and the software deletes neither before
  a disk check.
- The bin invalid, the tmp 6: the tmp.
- 6 and 5: the bin.
- No bin, a tmp of major 2: a v1 device has no transfer data; a v1
  software refuses to write.

**The builder's precedence** (2.9):

- The walk (s, t) and T (s, t): T.
- T (s, t − 3,600) under a skew of +3,600: T.
- T (s, t − 2), no skew: qfp; equal: T, and the confirmation is saved.
- T (s + 1, t) and a Scanned D record (s + 1, t): D.
- T UNREADABLE without FROM_API, and the device reads the tags: D.
- A Scanned D record of an older parserVersion, no T: path names, and
  the file goes Pending.
- The walk's `Acme/x.mp3` against T's `ACME/x.mp3`: no T match on the
  device.

**The software's matching** (2.10.2, 2.11, 2.8.5):

- The ledger has `ACME/Hits/01.mp3`; the card lists `Acme/Hits/01.mp3`
  with an equal size and time: unchanged, and the ledger takes the
  spelling `Acme/…`; no `01 (2).mp3`.
- A plan's write target that is on the card but not in the ledger is
  replaced in place.
- On macOS, a listed `Cafe` + U+0301 is recorded in NFC (`C3 A9`), hash
  F1B24FC757494F2B.

**The album folder** (2.14.1): `/music/Artist/Album/CD1/01.flac` takes
the thumbnail key of `/music/Artist/Album` (B1F7E69FBD466B59), not of
`/music/Artist/Album/CD1` (D9FA96D903A5A10C); `/music/Artist/x.mp3`
takes `/music/Artist` (4296E8541CC39B71).

**The extension** (2.8.3): an Opus stream the server stores as `x.ogg`
goes on the card as `x.opus`, its bytes unchanged, convertedTo 0.

### 2.19 Sizes at 20,000 tracks (ESTIMATED)

| File | Size |
|---|---|
| `manifest.bin` | under 4 KB |
| `tags-<g>.bin` | about 6.9 MB: RECS 1.44 MB, STRS about 2.4 MB (names about 31 B and runs about 87 B per track), HIDX 0.24 MB, ORIG 1.6 MB, OSTR about 1.2 MB. The device reads FOLD, RECS and STRS: about 3.9 MB, 2.3-3.3 s at 1.2-1.7 MB/s, during a build only |
| `autodj-<g>.bin`, K = 100 | about 6.7 MB (DJNB 6.0 MB); one 300-byte row read per pick |
| `/.mstream/thumbs` | about 39 MB for 1,800 albums |
| `/.player/tags.bin` | about 2.3 MB when the software covers every file (status rows, names); about 4.1 MB when the device scanned everything (HIDX, 0.24 MB, included) |

All of it is small next to the music (about 700 MB per 100 tracks).

---

## 3. The device side: boot, scanner, builder, budgets

The player's half. It reads part 2's files and never writes `/.mstream/`;
everything it keeps is in `/.player/`. Figures carry the research's
labels; device times are to be measured in milestones L0-L5 (part 6).

### 3.1 What changes, in short

- **Boot stops walking the card.** `Library::begin()` loads
  `/.player/library.idx` when its inputs still match, and the card is
  checked by a background walk after the UI is up. After a transfer, the
  boot builds from T without walking. Only a card with no records at all
  (a card-reader fill, or the first boot of this firmware) walks at boot.
- **The walk lists once, with sizes and FAT times**, through FatFs's
  `f_readdir` (`FILINFO` carries both), one folder at a time, and skips
  folders whose digest is unchanged.
- **A PSRAM sector cache under FatFs** (128 KB: 3.2.7) makes the walk and every
  open fast on a big card. It is the precondition for 20,000 tracks.
- **The scanner reads only files no fresh record covers**: on a
  transfer-filled card, none. It shares one card worker with the
  thumbnails, runs on battery and while playing, and yields to covers,
  scrolling, track changes and a low PCM ring. A journal makes it
  resumable after any power-off.
- **One update step** rebuilds the index for every caller: it frees the
  old index first (two don't fit at 20k), and the queue follows through
  `queue.txt` instead of an in-PSRAM text copy.
- **Stage A** shows tag names on folder albums (part 5).

### 3.2 Boot

#### 3.2.1 Today (READ)

- `Library::begin()` runs in `setup()` before the UI (`src/main.cpp`).
- **Every boot** walks `/music` to hash the paths (`signatureWalk`;
  FNV-1a 64 over the paths only, `src/app/Library.cpp`). **A changed card**
  walks a second time to build.
- The walk is POSIX `opendir`/`readdir` by full path
  (`LocalStorage::forEachFile`). FatFs resolves every open from the root
  (`FF_FS_RPATH 0`), and the SD driver caches nothing and takes the SPI
  bus once per single-sector read (`sd_diskio.cpp`).
- At the user's shape (705 entries in `/music`, about 56 directory sectors
  per open), the walk costs **89-148 s at 20k** (ESTIMATED, calibrated at
  0.6-1.0 ms per sector on the device's own card; metascan section 5.3).
  On today's 77-track card it is 52-71 ms (MEASURED).

#### 3.2.2 The new boot

1. **Read the root:** `/.mstream/manifest.bin` and `manifest.tmp` (2.5.4),
   then the named MPTG companion's header and directory: three small
   reads, a few ms. Result: (cardId, generation, commitId, T's headerCrc),
   or no transfer data.
2. **Read `library.idx`'s header:** its version, the rules version, the
   **hard inputs** (the transfer's identity above) and the **soft inputs**
   (D's headerCrc and its journal's sequence).
3. **Decide:**

| `library.idx` | Records on the card | At boot | Then, in the background |
|---|---|---|---|
| v6, hard inputs match, and the build-at-boot marker `/.player/build.req` present (a build was deferred: 3.4.2) | any | Compact the journals if any. **Build from the records** behind the boot screen, on a fresh heap, as in the next row; the marker goes after the save. Out of PSRAM even here: **load it** as the next row does (stale, but a library), the marker goes, and that session's short memory checks don't write it again (as built: 3.9). | The validation walk (3.2.3). |
| v6, hard inputs match, no marker | any | **Load it:** 1.8 MB, 1.1-1.5 s at 20k (ESTIMATED). No walk. | The validation walk (3.2.3). If the soft inputs differ (the scan went on after the last build), resume the scan; rebuild at its end. |
| v6, the transfer's identity differs (a transfer happened, or `/.mstream` is gone or damaged) | T valid, or D only | Compact the journals if any (0-3 s). **Build from T (if valid) and D** behind the boot screen, a new T's listing standing in for the walk (2.9): about 7-9 s at 20k (ESTIMATED from UI-SPIKE's measured rates: 3.5-5 s of reads, 2.2-2.6 s of adds, about 1.3 s to finish), then the save. Without T, D's rows for files T covered turn Pending. | The walk confirms; files it finds changed go Pending; the scan reads the Pending files. |
| an older rules version, v1-v5 (today's is v5), missing or corrupt | some | Build from the records. No walk. | The walk. |
| v1-v5, missing or corrupt | none: a card-reader card, or this firmware's first boot on a 0.7 card | **Walk now**, with a progress line, into a path-named index and a D of all-Pending entries: about 9-11 s of walk at 20k with the sector cache (89-148 s without), plus about 3.5-4 s to build; about 70 ms on today's card. | The scan (3.3). |
| any | NoMemory | As today: no library; the built-in tracks still play. | none |

- The queue is then restored as today (`QueueStore::restore()`).
- **A power cut mid-save:** if `library.idx` is missing and `library.tmp`
  loads with a good checksum, it is used and renamed (power went between
  the remove and the rename). `queue.tmp` already follows this rule.
- `LibraryIndex::Load::Stale` no longer happens at boot: the path
  signature isn't computed any more.
- **`device.txt`** (2.15) is rewritten here when its content would change
  (a new firmware): one small write.
- **A plan** present (`pending.bin`, or a valid `pending.tmp`: 2.12.5):
  the Library tab's status line says the last transfer didn't finish.
- **Cut renames** of the device's own files are settled here, before
  anything is written (2.12.6).

#### 3.2.3 The validation walk (the card worker, every boot, about 2 s after the UI's first frame)

**The lister.** FatFs `f_opendir`/`f_readdir` on `"0:/music/..."` (the SD
driver mounts drive `"0:"`). `FILINFO` gives `fsize`, `fdate` and `ftime`
in the same directory read, so there is no `stat()`. The LittleFS
fallback keeps today's POSIX walk, with `st_mtime` packed into a FAT time.

**One folder at a time:**

1. Read its entries into a PSRAM scratch of at most 64 KB, then close
   it: one DIR open at a time.
2. FatFs's `DIR` and `FILINFO` objects live in PSRAM, allocated once per
   job; FatFs puts its long-name buffer (512 B,
   `CONFIG_FATFS_LFN_STACK`) on the caller's stack for each call, which
   the worker's stack budget counts (3.3.4).
3. Sort the entries into the canonical order (2.6.8). `/music` with 705
   children is about 28 KB. **A folder bigger than the scratch** (the
   contract allows about 2,000 entries, 2.8.4, and a listener's flat
   `Singles` folder can hold more) is listed in passes: each pass reopens
   it and keeps the smallest names greater than the last one emitted, as
   many as the scratch holds. Names in a folder are unique, so the passes
   emit every entry once, in order: a 3,000-file folder (about 135 KB of
   entries) takes 3 passes, about 0.3-0.6 s each (its 560 or so
   directory sectors are more than the sector cache holds). **The walk
   never emits a folder out of order**, since `walk.jnl`, the compaction
   and the build are all merges of sorted runs.
4. Its digest: FNV-1a 64 over (name, size, fatTime) of its audio and image
   files, and its count of other files.

**Comparing with what the device knows:**

- D's device-private folder table (`DFLD`) is streamed in step with the
  walk through an 8 KB buffer: both are in pre-order.
- **Equal digest:** nothing to do for that folder. This is the normal
  boot.
- **Different digest:** merge its files against D's records for that
  folder (a seek to the folder's range).
- **The first walk after a commit** (D's walk identity differs from the
  root's): merge against T's records too, one near-sequential pass over
  T's FOLD, RECS and STRS (about 3.9 MB, 2.3-3.3 s at 20k).

**T's records, per file** (2.9 rule 1): size and time equal is a match;
size equal and time different is *doubtful*. A doubtful file is written
to `walk.jnl` as Doubtful (path, Δ), not held in RAM (all 20k files can
be doubtful when a PC shifted every stamp), and its Δ goes into a
histogram of 256 slots (2 KB, `cardcontract::SkewHistogram`): exact
counts while at most 256 distinct values came, a Misra-Gries summary
past that (any Δ that half the pairs have is still in it). At the walk's
end the skew (2.3.4) is computed; when the summary was needed
(`needsRecount()`, a card with more than 256 distinct retouches), one
pass over the Doubtful entries first counts its candidates again exactly,
so the skew is 2.3.4's whatever the walk's order. Then one pass over the
Doubtful entries matches those whose Δ is the
skew, and gives the rest a qfp check (an open and two 4 KB reads, about
8 ms with the cache, ESTIMATED). A match is saved in D as a
confirmation, so it is paid once per file, not per boot. Worst case (a PC
that converted each stamp differently): about 2.7 min of checks once at
20k.

**Output:** `walk.jnl`, at most two sorted runs: the walk's Added (path,
size, time, Pending or Software), Changed, Gone, Doubtful, FolderCover
(the best image's name, rank, count, size, time, and whether T's ledger
lists it) and Confirmed (the commit); then the doubtful files'
resolutions (Confirmed or Changed). Nothing is written when nothing
changed.

**Cost:** about 9-11 s at 20k with the sector cache, 89-148 s without
(ESTIMATED). Each `f_readdir` holds the FatFs volume lock (`FF_FS_REENTRANT`)
for a sector or two: about 35 µs cached, 0.6-1.0 ms uncached.

#### 3.2.4 The PSRAM sector cache

**`lib/core/SectorCache` (portable; built, 3.2.7):** an LRU of single 512 B
sectors, 256 entries (128 KB of PSRAM: 128 sit at the knee of the user's
`/music`, 3.2.7), hashed by LBA. Multi-sector reads (file data) bypass it;
writes go through, and a cached sector a write covers takes its bytes (or
is dropped, if the write failed); it is cleared at mount.

**Installed (firmware; built in N10, `storage/SectorDisk`: 3.8):** a
diskio wrapper registered with
`ff_diskio_register` right after a successful `SD.begin()`, forwarding to
the SD library's non-static `ff_sd_initialize`, `ff_sd_status`,
`ff_sd_read`, `ff_sd_write` and `ff_sd_ioctl`.

- `sdcard_init` registers the stock driver, so the wrapper goes in after
  every mount.
- `storage.begin()` runs before `audio.begin()`, so no other task is
  inside a disk call during the swap.
- `probeCard()` mounts only to look and ends with `SD.end()`: no wrapper
  there.
- FatFs calls `disk_read` under its per-volume mutex; with one volume the
  cache needs no lock of its own.

**What it speeds up:** every path lookup, the walk, the scanner's opens,
the decoder's FAT-chain reads on a seek (fast seek is off), a track's
open at a gapless advance. The decoder's longest wait for the volume lock
falls from about 50 ms to about 2 ms (ESTIMATED, metascan section 5.2).

#### 3.2.5 Boot and background costs (ESTIMATED; L0-L2 measure them)

| Step | 77 tracks (today's card) | 20k tracks |
|---|---|---|
| Mount, the root, the index header | about 0.2 s | about 0.2 s |
| Load `library.idx` | 5 ms (MEASURED) | 1.1-1.5 s |
| Restore the queue (`queue.txt`) | ms | 0.1 s (a short queue) to 0.4-0.5 s (a full queue since the cap: 5,000 lines, about 0.4 MB, `findTrack` per line; 1.5-2 s for the 20k lines before it) |
| **To a browsable library** | **under 0.5 s** | **about 1.5-3.5 s** |
| Instead, a build at boot (after a transfer, or a deferred one), before the UI | under 0.1 s | about 7-9 s, plus the journals' compaction (0-3 s) |
| Background walk, cached | under 0.05 s | 9-11 s, plus 2.3-3.3 s on the first walk after a transfer |
| qfp checks (only after a PC wrote times the skew rule can't explain) | none | at most about 2.7 min once |
| The update step's pause (3.4.2) | under 0.1 s | about 9-12 s idle, 11-14 s while an MP3 plays (a 20k-line queue; a full one since the cap is about 1-1.5 s less) |
| MPDJ's DJNB check, once per commit, on the card worker (3.4.2) | none | 3.5-5 s of reads (6 MB) |

If L2 measures more than 3 s for the queue, a binary fast path (track ids
saved with the index's build stamp, used when the stamp matches) can come
later; `queue.txt` stays the fallback. The totals above still count a
20k-line queue; the queue's cap (3.5) takes up to about 1.5 s off them,
kept as margin until L2 measures.

#### 3.2.6 As built (N5)

`lib/core/CardWalk` is 3.2.3's walk, host-tested (`test_card_walk`, on
the fake FAT trees of `test/support/FakeFat.h`), called since N10 by the
card worker (3.8: its FatFs lister is `storage/CardFat`). What the code decided
where 3.2.3 left room:

- **The lister** (`cardwalk::Lister`, 3.6's `IDirLister`): `openDir`,
  `next` and `closeDir` give each entry's name, size and FAT time, as
  `f_readdir`'s `FILINFO` does, one folder open at a time; `openFile`
  reads a file for its qfp. A missing `/music` is a card with no library;
  any other failed listing stops the walk, and nothing it gave counts.
- **The scratch is a stack.** A pass keeps the smallest keys after the
  last one used (a heap of offsets, the entries packed from the top), so
  a folder of any size comes in order through any scratch from 2.4 KB
  (`kMinScratch`: one 268-byte entry for each of the nine levels). Each
  level leaves room for the levels below it, and a folder's waiting
  subfolders take at most half of its pass's room. A big folder's digest
  is found in passes first and its files listed again only if it
  changed; one that is merged anyway (the first walk after a commit, or a
  folder D doesn't list) is digested and merged in the same passes.
  Measured on the host: a 3,000-file folder takes 3 passes in the
  device's 64 KB, as 3.2.3 said, and 25 in 8 KB.
- **The digest's bytes:** FNV-1a 64 over each audio and image file in
  name order (its name, a 0 byte, its size and FAT time as u32 LE), then
  the count of the other files (u32 LE). Device-internal.
- **D's rows** carry the status, the confirmation and the qfp the device
  read for the file at its size and time (the record's `qfp`, which 2.6.4
  lets the device fill). A later commit whose T has the same fingerprint
  for a doubtful file settles it without a read, so a PC that converts
  stamps unevenly costs 3.2.5's 2.7 min of checks once per card, not once
  per commit. A qfp read that fails leaves the file's row without T and
  says so in the walk's summary (`Summary::unsettled`): D doesn't take
  the walk's commit (3.3.7), so the next boot's walk is a first one and
  reads it again (2026-10-07 review: kept at the commit, the next walk
  found the row at the file's size and time and never asked T again).
- **Which folders D lists:** those with audio at or below them (a folder's
  row waits until audio turns up below it): FOLD's folders. A folder of
  images alone (a `Scans` folder) has no row; each walk lists it and
  writes nothing.
- **The skew's pairs** are every file T has a record for, covers included
  (2.3.4); a pair that isn't an audio file's doubt goes to the sink as a
  count-only doubt, so the recount past 256 distinct deltas is exact.
- **Covers:** a folder's best image is the lowest `imageRank()`, ties to
  the first by name (as `LibraryIndex` elects in canonical order). It is
  owned (2.14.3) when T has a record of its size at its path; the time
  isn't asked.
- **T** is streamed on the first walk after a commit (every file asked,
  one pass) and read through HIDX at the same commit (the changed files
  and the merged folders' covers only). A T that fails a check at its end
  fails the walk (`Error::Transfer`), to be run again without it. A T
  record UNREADABLE without FROM_API gives the file the row it would have
  without T (Pending: the scan reads it); a fresh one turns a Scanned row
  Software (U8).
- **N4's side** is two interfaces: `Known` (D's folders in pre-order with
  their digests, each folder's rows by name) and `Sink` (`walk.jnl`: the
  walk's rows and gones, the doubts read back after it, the settled rows,
  then the summary with the skew for DHDR; `abort()` drops a failed
  walk's output).
- **Memory and steps:** the walk is about 7 KB, a streamed T's walker
  3.9 KB and its 8 KB of buffers, the scratch 64 KB: PSRAM, never the
  worker's stack. A step lists at most one folder (or one pass of a big
  one) or reads at most one qfp.

#### 3.2.7 As built (N8)

`lib/core/SectorCache` is 3.2.4's cache, host-tested (`test_sector_cache`,
`test_fat_model`), installed since N10 (`storage/SectorDisk`, 3.8). What
the code and the model decided:

- **The model.** ChaN's FatFs R0.15, the revision ESP-IDF 5.5.5 carries,
  is vendored for the host only in `test/support/fatfs` (U14: from
  elm-chan.org, `ffunicode.c` cut to its CP850 table), configured as the
  Core2's sdkconfig builds it: long names on the stack, UTF-8, code page
  850, no relative paths, no fast seek, a buffer per file, TRIM on. It runs
  on sparse RAM disks (`test/support/FatModel.h`), counting FatFs's
  `disk_read` calls (the stock SD driver's card reads: one CMD17 each) and
  what reaches the card through the cache. `tools/fatmodel.py` runs it on
  N11's card (tools/synthcard.py's plan, seed 1, or a tree it built: the
  same figures): 19,410 audio files to open, a `/music` of 1,659 entries
  in 104 sectors (the user's: 1,628 in 102), FAT32 with 32 KB clusters,
  everything created in the copy's order.
- **The research's counts hold.** FatFs reads exactly what metascan's
  model counts: at each level of the path the sectors up to the name's
  entry, plus a FAT sector for each cluster the scan crosses
  (`test_fat_model` checks it file for file on a 20k tree of
  `LibrarySynth`'s). On N11's card a lookup (an open, a stat, an opendir)
  is 58.0 card reads on average (p90 99, max 116; metascan: 56), 35-58 ms
  at the calibrated 0.6-1.0 ms a read. The probes L0 opens, at entries 1,
  353 and 703 of `/music`, read 4, 55 and 108. 3.2.3's walk reads 150,742
  sectors (90-151 s; today's nested walk 153,241; metascan: about 148,000
  and 89-148 s).
- **256 sectors, not 128** (128 KB; the block with its links and its hash
  is 135,168 B). Each lookup scans `/music` from its first sector, so an
  LRU smaller than `/music` evicts, at each lookup, what the next one
  needs. Card reads, warm (the first 2,000 opens left out) or from a mount:

| Cache | A random open: mean, p99 | 3.2.3's walk | The scan's opens and 4 KB heads, per file |
|---|---|---|---|
| none | 58.0, 109 | 150,742 (90-151 s) | 59.4 |
| 64 sectors | 41.7, 108 | 105,599 (65-107 s) | 40.8 |
| 128 sectors | 4.8, 27 | 7,355 (9.4-12.4 s) | 1.75 |
| 256 sectors | 3.5, 8 | 7,276 (9.4-12.3 s) | 1.75 |

  The knee is `/music`'s own size: 96 sectors give 18.6 a random open, 112
  give 7.1, 160 give 3.9. The user's `/music` sits just under 128's knee,
  and grows a sector for about every 7 artists; 256 moves the knee to
  about 1,500 artists for 64 KB more (3.5). What a random open still reads
  at 256 is its artist's and album's own folder sectors, about 3; a walk
  or a scan reads each folder's sectors about once (3.2.3 said 9-11 s for
  the walk; the model says 9.4-12.3 s at 35 µs a hit). The times are the
  research's per-read figures; L0 measures them.
- **What it keeps.** Single-sector reads (FatFs's folders and FAT, a
  file's partial sector) are kept, least recently used out first; a read
  of more than one sector goes to the card in one call and changes
  nothing; a failed read keeps and evicts nothing. A write goes to the card
  first; each cached sector it covers then takes the written bytes, or is
  dropped if the write failed (3.2.4 said "invalidate": refreshing keeps
  the folder and FAT sectors that a journal's appends rewrite, and equals
  a drop and a read back). A write never adds a sector. `invalidate()`
  serves a TRIM (FatFs trims freed clusters, `FF_USE_TRIM`), `clear()` a
  mount.
- **The checks.** `test_sector_cache` drives random reads, writes, writes
  that fail part way, failed reads, invalidations and clears against a
  reference LRU and the device's bytes, for 1 to 64 sectors: the same
  sectors in the same order after every step, the device asked exactly at
  the reference's misses, every byte the card's. `test_fat_model` runs the
  same random file work (folders, writes in random chunks and past the
  end, reads, deletes, moves, truncations, listings, remounts, the cache
  dropped behind FatFs's back) through FatFs on two RAM disks, one behind
  a 24-sector cache and then a 4,096-sector one: the same results, the
  same bytes and the same card image. Its TRIMs scramble their sectors and
  its allocation is sent back to the freed clusters, so a cache that kept
  a trimmed sector fails it (checked by breaking the wrapper's TRIM).
- **For N10's wrapper:** `clear()` at every mount, before
  `ff_diskio_register`; `CTRL_TRIM` calls `invalidate()` and then the SD
  driver; anything that writes the card around FatFs must invalidate too
  (nothing does today: `SD.writeRAW` isn't used). FatFs's volume lock
  serialises the cache (one volume). A lookup is one `f_open`, `f_stat` or
  `f_opendir`; how many an Arduino `File` open makes through the VFS is
  L0's to count.

### 3.3 The scanner

#### 3.3.1 Which files it reads

An audio file is scanned only when all three hold:

1. no T record matches it (2.9 rule 1, after the walk's confirmations),
   or the one that matches is UNREADABLE without FROM_API;
2. no D record of the same size and time, the current parser version and
   the current rescan epoch;
3. it isn't marked Unreadable at the same size and time (a failed parse
   isn't retried until the file changes or the listener asks).

Images and other files are never parsed; folder cover facts come from the
walk.

**Per file** (metascan section 5.1-5.2): an open, 1-3 reads of 4 KB, a
close: 10-60 ms. `TagScan` reads through a FatFs `Source` (`f_lseek` +
`f_read`); its 4 KB buffer and its record (about 6 KB at the limits of
2.3.6: eight single values of 255 bytes and four lists of 1,023, each
with its NUL) are in PSRAM, and so is the `FIL`. In this build a `FIL`
is about 4.1 KB, since it embeds a sector buffer (`FF_MAX_SS` is 4096 through
`CONFIG_WL_SECTOR_SIZE`, `FF_FS_TINY` 0 for the per-file cache, no
`CONFIG_FATFS_USE_DYN_BUFFERS`): on the worker's 6 KB stack it would
overflow at the first file. One `FIL` per worker is allocated once and
reused, for the scan and the qfp checks alike. (Today's Thumbs worker
opens through POSIX `open()`, whose VFS allocates the `FIL` with PSRAM
preferred.) Durations come from the existing helpers
(`progress::mp3HeaderDurationMs` with the LAME trim, `flacDurationMs`;
Opus from the last granule). Pictures are located, never decoded or
kept (inside a v2.2/2.3 tag under tag-level unsynchronisation a
picture's bytes are read through, the only way to find the frame after
it: 3.3.8). The reading rules are part 5's.

#### 3.3.2 `tags.bin`, the journals, compaction, resume

| Item | Rule |
|---|---|
| **`/.player/tags.bin`** | MPTG source 1 (2.6), in canonical order, one record per audio file the last walk saw, plus device-private sections (their layout is the device's own): `DSTA`, a status per record (**Software**: a T record confirmed at the current commit, the row carrying size and time only; **Scanned**: a full record; **Pending**; **Unreadable**), with a bit for "confirmed by qfp", and in its own header the number of rows that aren't Software and of the folders on their paths (the builder sizes the index from them when T lists: 3.4.4); `DFLD`, per folder: the digest and the cover facts; `DHDR`: the commit the last walk compared against, T's skew, the rescan epoch. Size at 20k: about 2.3 MB if every file is Software, about 4.1 MB if every file is Scanned (HIDX included). |
| **`tags.jnl`** | Chunks: a magic, a sequence, the headerCrc of the `tags.bin` it extends, a count, the records **sorted into canonical order** before the append, a CRC-32. One chunk every 100 files or 5 s: 15-20 ms per append (MEASURED for small writes). A torn last chunk fails its CRC and is dropped; a chunk for another `tags.bin` was already merged and is dropped. So the journal is a sequence of sorted runs, about 30 at 512 KB. |
| **`walk.jnl`** | The last walk's changes, at most two sorted runs (3.2.3). |
| **Compaction** | When `tags.jnl` reaches 512 KB (about 3,500 records), at a scan's end, and before any build: a **streaming k-way merge** into `tags.tmp` of `tags.bin`, `walk.jnl`'s runs and `tags.jnl`'s chunks, each run read through a 1 KB buffer (about 40-50 KB of PSRAM in all, nothing read whole); a path in several runs takes the newest (a later chunk over an earlier one, the scan's over the walk's over `tags.bin`'s, as before). A full 20k scan compacts about 6 times: about 12-24 s of writes at 0.5-1 MB/s, in the background (ESTIMATED). |
| **Atomicity** | Write `tags.tmp`, `f_sync`, remove `tags.bin`, rename, remove the journals. At boot, no `tags.bin` and a valid `tags.tmp` means rename it; a journal whose base doesn't match is dropped. |
| **Resume** | The to-do list is never saved: it is D's Pending entries minus the paths in `tags.jnl` (a hash set in PSRAM, at most 512 KB of journal). After a power-off at most the last unflushed chunk (100 files or 5 s) is read again. |
| **Rescan** | The parser version and the rescan epoch are in D's header. A firmware with a new parser version, or a Rescan, turns older Scanned records back into Pending in the background; the index stays usable meanwhile. |

#### 3.3.3 Priority

0. **Covers come first:** they are on screen, and Thumbs' rule today is
   that the rows on screen come first.
1. **The playing track, if Pending:** scanned at once (10-60 ms). Its tags
   reach Now Playing through a one-slot overlay in `TrackCatalog` until
   the next build (metascan's early value).
2. **The queue:** the next 3 entries, then the next 200.
3. **What the Library tab shows:** a hint from `LibraryPage` (the album or
   folder open).
4. **Everything else**, in D's canonical order.

**New files the walk finds** aren't in the index, so they can't be
queued or browsed until a build. If the walk adds 200 files or more, or
the scan's estimate is over 60 s, the builder runs once right after the
walk (the new files appear with path names), and again at the scan's
end (part 7, U11: decided so).

#### 3.3.4 Yielding (a portable `ScanScheduler`, host-tested like `test_idle_policy`)

**One card worker, shared with the thumbnails.** It generalises Thumbs'
worker: core 1, always below the decoder's 2; a 6 KB internal stack that
exists only while there is work, gone 3 s after the last step. A step is
one file, or one folder of the walk. Its jobs run one at a time, so a
build, a compaction and a scan never overlap (3.4.2).

**Its priority, per job.** The SD driver's reads busy-wait the CPU
(`sd_diskio.cpp` to `SPIClass::transferBytes`, polled), so a step at the
loop's priority 1 time-slices with the loop for its whole length, and
back-to-back steps would halve the loop's share for minutes. So:

- **priority 0** for the walk, the scan, the compaction and the DJNB
  check: they run in the time the loop and the decoder leave, and the
  loop preempts them whenever it is ready;
- **priority 1** only for a cover job whose row is on screen (Thumbs'
  rule today), and for the update step's build, which the listener is
  waiting for (3.4.2);
- a loop that never blocks would stall priority-0 work; L3 measures the
  scan's rate on the Dance page and Now Playing, and L3 and L5 record
  the loop's `pass_max` during a scan.

**Before each step, in order:**

1. A cover job is waiting: do it first.
2. A list is moving: start nothing.
3. The PCM ring is below 50%: wait.
4. An underrun since the last step: back off 30 s.
5. The track-change window, from the decoder's end of file on track N
   (it then opens N+1 into the same ring, which holds up to 1.49 s;
   GAPLESS.md section 2.1) until 2 s into N+1: wait.
6. A seek, and the 2 s after it: wait.
7. A Bluetooth pairing or link setup (`pairingUnderWay()`): wait.

**Cost** (ESTIMATED; L3 measures): a full 20k scan takes 2.5-2.9 min idle
with the cache, 3-6 min while playing (14-23 min idle without the cache).

**Risk:** the worker's 6 KB stack is held for the whole scan, and
Bluetooth mode has the least internal RAM. Nothing large lives on it:
the `FIL`, `DIR` and `FILINFO` are in PSRAM (3.2.3, 3.3.1), and what
remains is TagScan's about 1 KB, FatFs's 512 B long-name buffer and the
call frames. L3 measures the stack's high-water mark and the lowest
internal free during a scan in Bluetooth mode.

#### 3.3.5 Battery and power (the user's choice: on battery, while playing)

- **Cost:** about 1% of a 390 mAh charge for a full 20k scan with the
  cache (ESTIMATED, metascan section 6.4).
- **Low battery:** pause below 10% when not charging; resume on USB or
  above 15% (part 7, U13: decided, 10%).
- **No idle-off blocker for scanning.** On USB the idle power-off is
  already blocked (`IdlePolicy`'s `Usb`); on battery a power-off just
  interrupts the scan and the journal resumes it. The shutdown path
  flushes the scanner's buffered chunk next to the queue.
- **A new blocker, `LibraryWrite`**, next to `QueueWrite` in
  `IdlePolicy::Blocker`: it holds while a build saves `library.idx` or a
  compaction renames, so the power never goes mid-rename. It lasts
  seconds. (As built, it holds for the whole update step and the whole
  compaction: 3.3.9.)

#### 3.3.6 Progress, Rescan, the console

- **The Library tab:** one status line while active ("Checking the
  card…", "Reading tags 1,234 / 19,410", "Updating library…", "The last
  transfer didn't finish"), redrawn at 1-2 Hz (at 10 Hz the redraw cost
  5%, MEASURED).
- **Toasts:** at the start ("Found 12 new tracks") and the end ("Library
  updated").
- **The settings (Output) tab, a Library row:** the track count; how many
  have tags from the transfer, read here, or none; **Rescan tags** (the
  device's own records only; transfer records are refreshed by the next
  transfer). (Built in N12, 3.9: "Library: 19,410 tracks" over "18,000
  from the transfer, 1,400 read here, 10 without tags", shorter forms
  when that doesn't fit, a Rescan pill that asks first.)
- **The console:** `g` (today's report plus the scan state), `gs` (the
  scan's status), `gr` (rescan tags), `gr!` (rescan everything, transfer
  files included, as a diagnostic), `gt</music/...>` (one file's records
  from both sources and the winner), `gw` (walk now), `gb` (build now),
  `gv` (Verify: qfp of every software-owned file against T). `g0` stays
  "walk and rebuild".
- **Texts:** the placeholders in `lib/core/UiText.h` and the empty state
  ("Put folders in /music/Artist/Album/") are reworded: tags are read now.

#### 3.3.7 As built (N4)

`lib/core/TagStore` is 3.3.2's store, host-tested (`test_tag_store`)
through a file interface of its own (`tagstore::Fs`: FatFs on the device,
N10's glue), called since N10 (3.8): the boot opens it, the scan writes
through it, the card worker compacts it, a build reads it. N5's walk reads and writes it through
`lib/core/TagStoreWalk` (`KnownD`, D as `cardwalk::Known`; `WalkSink`,
walk.jnl as `cardwalk::Sink`; `test_tag_store_walk` runs the real walk on
fake FAT trees into it). What the code decided where 3.3.2 left room:

- **D's sections**, after HIDX and not REQUIRED (N1's reader skips them):
  `DSTA`, a 16-byte header (the records; the rows that aren't Software and
  the FOLD entries on their paths, 3.4.4's `ownCounts()`) and a byte per
  record (`LibraryBuilder::Status` in bits 0-1, "confirmed by qfp" in bit
  2); `DFLD`, an 8-byte header and per FOLD entry N5's folder row (its
  digest; its best cover's name, rank, size, time and ownership; its
  counts of audio, images and other files: 31 bytes and the name); `DHDR`,
  48 bytes (a version, the commit the walk compared against, T's skew, the
  rescan epoch). A Software or Pending row's record holds the size, the
  FAT time and the qfp the walk read (2.6.4 lets the device fill qfp; N5
  saves it so a confirmation is paid once); an Unreadable one the
  UNREADABLE flag. FOLD lists the folders with audio at or below them, as
  N5 does. The rest is the contract's MPTG: what a compaction writes is
  byte for byte what N1's writer makes of the same rows (a test).
- **One parser and one epoch per D:** the header's parserVersion and
  DHDR's epoch are its readings'. A compaction under another parser, or a
  Rescan (`compact(true)`, the next epoch), turns Scanned and Unreadable
  rows Pending (their size, time and qfp kept), and so do chunks read
  under another.
- **`tags.jnl`:** a chunk is a 32-byte header (magic `MPJC`, the sequence
  from 1, D's headerCrc, the count, the payload's length, the parser, the
  rules, the epoch), the records sorted into canonical order (a path
  added twice keeps its last), and a CRC-32. Boot checks each chunk whole
  (its CRC, its order, each run's shape) up to the first that fails; the
  next append cuts the tail off there. A journal whose first chunk names
  another D was merged already, and the next append replaces it.
- **`walk.jnl`** is the target of N5's `cardwalk::Sink`: a 64-byte header
  (magic `MPWJ`, D's headerCrc, the commit, and `tags.jnl`'s last
  sequence when the walk began), then CRC'd blocks of File, Gone, Doubt,
  Folder and FolderGone entries, each run closed by an End block with T's
  skew. Run 1 is the walk's rows and its doubts, read back after it; run 2
  its settled rows. Each kind keeps its own order (files and gones
  canonical, folders in pre-order, doubts canonical), the kinds
  interleaved as the walk gives them. A doubt changes no row: the walk
  gives the doubtful file's row without T first. Run 1 without its End is
  dropped (the card is walked again); run 2 without its End leaves run 1's
  rows (the doubtful files without T). DHDR takes the walk's commit and
  skew from a whole walk whose doubts were all settled. A walk that
  leaves one unsettled (run 1 alone with a doubt in it, or a qfp read
  that failed, which run 2's End says) leaves DHDR unwalked, so the next
  boot's walk is a first one: it merges every folder against T and
  settles the doubts again. (2026-10-07 review: such a walk at the same
  commit used to leave DHDR at that commit; the next walk was then one at
  the same commit, which found the doubtful files' rows at their sizes
  and times and their folders' digests D's, and never asked T again until
  the next transfer.) An unchanged card's walk writes
  nothing; a failed one is removed (`abort()`). `WalkSink` closes run 1
  when the walk reads its doubts back; the walk's skew comes with its
  summary, in run 2's End.
- **Which row wins:** per path, the newest in time: D, the chunks older
  than the walk (by the header's sequence), run 1, run 2, the newer
  chunks. A walk's row keeps a reading made at the same size and time
  (unless T now covers the file), and a qfp of that size and time. So the
  scan may run before and after a walk with no compaction between them,
  and a card changed on a PC is seen by the next boot's walk over the
  chunks the last session left. `TagStore::View` reads this merged view
  without writing (the scan's to-do: its Pending rows in canonical order),
  in the compaction's memory.
- **The compaction is two passes** over its inputs: the first counts each
  section (and writes HIDX's pairs to `hidx.tmp`), the second writes each
  at its place, a 512-byte buffer per section; then HIDX is sorted in
  passes over `hidx.tmp` in the memory the merge gave back (8 passes at
  20k), then DHDR, the directory and the header, a sync, and 2.12.6's
  rename. Its memory is fixed by its config: 66,220 bytes at the defaults
  (32 chunks, 512-byte buffers), the same with 32 chunks of records at
  2.3.6's limits as with 96 chunks (MEASURED on the host). `albumValues`
  and `artistValues` are exact to 128 values, then a linear-counting
  estimate (they only pre-size). D failing its checks partway through the
  first pass is left out (3.4.1): the journals alone make the new D, its
  DHDR unwalked, and the next walk compares the whole card.
- **The journal's limits:** 512 KB or 32 chunks asks for a compaction
  (`wantsCompaction()`, also when D was read by another parser), and
  `append()` refuses a 33rd chunk, so the memory holds. A journal of more
  (another firmware's) has the rest dropped by the compaction; their files
  stay Pending and the scan reads them again.
- **2.12.6, extended:** twins are `tags.xl1` to `tags.xl9`; repeated cuts
  without a disk check take the next free name, and with all nine taken
  the file isn't replaced until a disk check frees them. `settle()`,
  `prepareTmp()`, `replace()` and `collectTwins()` take any file's names,
  so `library.idx`, `queue.txt` and `device.txt` can use them
  (`library.idx` and `device.txt` do since N10; `LibraryUpdate`, N12).
- **The builder's inputs** (3.4.4): `BuilderRows` (DSTA, its CRC checked
  first: N2's walker doesn't know the section) and `BuilderFacts` (DFLD
  through a folder cursor) are `LibraryBuilder::DeviceRows` and
  `FolderFactsSource`.
- **Proven on the host** (a fake card whose rename is two directory
  writes and that records any cluster chain freed while another entry
  uses it): a session of walks, scans, compactions and a Rescan, cut at
  each of its 192 steps (every write, sync and remove, both directory
  writes of each rename) and booted three ways (every step before the cut
  on the card; the unsynced writes lost; the cut write half done). Each of
  the 576 boots finds the state before the step that was cut or after it
  (or, inside a walk, run 1 alone), reads it whole, and the rest of the
  session reaches the same end; 2,677 boots whose recovery was cut again
  do too; no cut frees a chain another entry uses, and the 9 cuts that
  left `tags.bin` and `tags.tmp` on one chain made twins. Through N5's
  walk: a first walk after a transfer whose stamps a PC shifted an hour
  (every file doubtful, settled by the skew) and its compaction, cut at
  each of their 41 steps and booted the three ways: the 123 boots (24 of
  them with run 1 alone) each find the card's files at their sizes and
  times, and the next boot's walk and compaction make the D the uncut
  ones made. And a walk at the same commit whose doubt (a time a PC
  rewrote, the bytes the same) its qfp settles, cut at each of its 42
  steps and booted the three ways (126 boots, 24 with run 1 alone), or
  with that read failing: the next boot's walk settles the file as T's,
  confirmed, and DHDR at the commit. After each cut every file is closed
  (an ASan run found `walk.jnl` left open when the cut failed its header's
  write; the fake now counts open files).
- **Measured (host) at 20k** (the user's shape, every file scanned, 100 a
  chunk): D of Pending rows 2.51 MB, of full records 3.84 MB (3.3.2 said
  about 4.1). The scan compacts 8 times: 71.0 MB read and 28.0 MB written
  in all, the last compaction 10.0 MB read (D twice, the journal twice,
  HIDX's passes) and 4.1 MB written. At 1.2-1.7 MB/s to read and 0.5-1
  MB/s to write that is about 10-16 s for the last compaction and 1.2-1.9
  min over a full scan (ESTIMATED), more than 3.3.2's 12-24 s since every
  compaction reads D twice. Levers if L3 finds it slow: a bigger journal
  (fewer compactions), HIDX's sort in more memory.

#### 3.3.8 As built (N6)

`lib/core/TagScan` is 3.3.1's reader, with part 5's small rules as pure
functions in `lib/core/TagRules`, host-tested (`test_tag_scan`), run since
N10 by the card worker through a FatFs `Source` (3.8). What the code decided where 3.3.1 and part 5 left room:

- **The reference** is mStream's reader: lofty 0.25 in relaxed mode
  behind mStream's ID3v2 repair pass, then mStream's selection rules and
  part 5. `tools/tagref` (Rust, lofty 0.25.1) reads the corpus that way
  into `expected.json`. Since the first value of a field wins, the
  device models the order of lofty's frame and item lists (its swap
  removals included), not only their contents. Where lofty fails the
  whole file, the reference record is UNREADABLE and the device reads
  the file itself (2.9). The known differences, all rare, are listed in
  `TagScan.h` and the corpus's README.
- **The read budget.** The tags are walked within 512 KB and 160 reads
  of the source; what follows the walk (the audio's first frame, the
  tail, the length, the values located) has a reserve on top (128 KB, 48
  reads). A walk the budget stops still ends in a record:
  `Result::Partial`, the fields found before the stop, never UNREADABLE
  (which 3.3.1's rule 3 would keep until the file changed). A Partial
  record is the device's reading (Scanned: N7 keeps it so, and counts it
  in the scan's log); a parser version that reads further reads it again. Two
  walks read more than headers: a v2.2/2.3 tag under tag-level
  unsynchronisation is read through (a frame's size counts its bytes
  resynchronised, so the next frame is found no other way; lofty reads
  the whole tag too), its bytes on top of the budget up to 8 MB, in
  sequential reads of the buffer (a 2 MB cover there is about 500 reads
  of 4 KB); and the page headers of an Opus comment packet past a
  picture are probes that count their bytes but not as reads (a 9 MB
  picture is about 190 pages of 64 KB). (2026-10-07 review: before, a
  cover past about 500 KB behind tag-level unsynchronisation made the
  file UNREADABLE, and an Opus picture past about 7.5 MB left a record
  with no fields and no length, as Ok.)
- **The tables** have room for 96 frames or comments a field may take.
  A v2.2 or v2.4 tag keeps no entry for a frame no field comes from
  (only v2.3's date removal reads such frames' places); a v2.3 tag gives
  such entries up for frames a field comes from; a full Vorbis table
  lets go of the comments that can't change the record (an empty value,
  a later value of a single field, a list's repeats and its values from
  the 18th distinct one, a number's later items). Before, 96 TXXX frames
  or 96 artists in front of the title hid the title.
- **An MP3's length** is read from the first frame lofty's rule finds
  (its Xing/Info, VBRI or LAME fields: `progress::mp3FrameDurationMs()`,
  no search of its own), in a 512-byte window, so it is the same
  whatever the buffer; else the bitrate over the audio.
- **Memory and stack:** the Scanner is about 10 KB (PSRAM), a scan about
  1 KB of stack; every read goes through one caller buffer (4 KB the
  design's, any from 512 bytes). `kParserVersion` is 1: no device has
  written a record yet.
- **Proven on the host** (`test_tag_scan`): the corpus (86 audio files:
  the crafted edges, and files past the tables' 96 entries) field for field
  against `expected.json`, the length within 100 ms, at nine buffer sizes
  from 512 bytes to 64 KB (sizes that aren't multiples of 512 included);
  every picture anchor read back to its image by an independent reader
  of each picCoding; generated files past the corpus's sizes (a 700 KB
  and a 2 MB cover behind v2.3 tag-level unsynchronisation, 1 MB behind
  v2.2's, a 9 MB Opus picture with its comments before or after it) read
  whole; a budget stop Partial with what was found (every corpus file
  under four tight budgets: Ok only with the whole scan's record); an
  MP3 whose second frame TrackProgress's own search would refuse: one
  length at every buffer; truncation at every byte; 10,320 mutated
  copies of the corpus files and 450 generated files (ID3v2.2-2.4 tags of up to 150 frames,
  FLAC and Opus with up to 300 comments, Opus pictures over pages of 16
  to 255 segments), each with its invariants, at two buffers, and under
  a random tight budget. A random differential (`tag_corpus.py --random
  N --out DIR`, `tagref --dir DIR`, then the reader over the same files
  in a scratch harness), 8,431 files at five buffer sizes, agrees with
  the reference on every file lofty reads; it found two differences,
  fixed and in the corpus: a TDRC whose text passed the value's buffer
  lost its year (`long_tdrc.mp3`), and an APE item's values after one
  past 255 bytes were lost (`ape_long_value.mp3`). Under ASan and UBSan
  (a Linux container, not the native environment): `test_tag_scan` and
  120,000 scans of a structured fuzzer of generated tags, clean.

#### 3.3.9 As built (N7)

`lib/core/ScanScheduler` is 3.3.3-3.3.5's scheduler, host-tested
(`test_scan_scheduler`), run since N10 by the card worker's loop side
(`app/CardTasks`, 3.8); N12's `LibraryUpdate` asks it for the build and the
save, and holds the rest from its fence to the save's end (3.9).
`IdlePolicy` has
the `LibraryWrite` blocker (`test_idle_policy`). What the code decided
where 3.3.3-3.3.5 left room:

- **The loop decides, the worker steps.** `update()` runs every loop
  pass, where its inputs are, and names the one step the worker may take
  now and at what priority, or why it waits. The worker takes only what it
  is handed, one step at a time (a cover; the build; the save; a folder of
  the walk, or a pass of a big one; a compaction; a file of the scan; the
  DJNB check), so no two jobs overlap. Thumbs' `loop()` hands out covers
  the same way today.
- **The order**, when the worker is free: the update step's build
  (priority 1; nothing here holds it, N12's safe point decides when it is
  asked); then nothing while a list moves, covers included (Thumbs' rule);
  a cover (priority 1, 0 while a list moves under it), whatever the audio;
  the save; then one job at a time, the first with work: the walk, a
  compaction, the scan, the DJNB check (priority 0). While the update step
  holds the worker (its fence to the save's end) the last four wait;
  covers don't. The scan's file comes from 3.3.3's sources in order: the
  playing track, the queue's next 3, the 200 after them, the Library
  tab's album or folder, the rest. The glue says which have a Pending file,
  once D's to-do is known (after the boot's walk).
- **The yields**, for the save and the background work, the first that
  applies (its name is the console's `gs` line): the battery floor (the
  scan and the DJNB check only); input in the last 0.5 s; playing with the
  ring below half; an underrun in the last 30 s; a decode pass over 40 ms
  in the last 5 s; a track change (from the decoder's end of file on the
  heard track to 2 s after the next is first heard: a gapless join, a
  start's or a skip's first audio); a seek, and the 2 s after; a pairing
  or a link being set up, and the 3 s after a link event (up or down).
  3.3.4's "an underrun since the last step" counts from the pass that sees
  it, and each window from the last pass its cause was seen. Three are new:
  input (a tap's redraw shares the SPI bus with the card); the long decode
  pass (G6 is 30 ms, and the 0.7.0 soak measured passes up to 30.5 ms on
  MP3 and 32.7 ms on FLAC with no scan, so over 40 ms the decoder waited
  for the card); and the settle after a link event (A2DP's start, AVRCP's
  handover). Each is a `Config` value (ESTIMATED; L3 and L5 measure).
  Playing, and the battery above the floor, hold nothing.
- **The battery floor (U13)** holds the scan and the DJNB check below 10%
  off USB, and lets them go on USB or above 15%; 10-15%, or a reading not
  known, keeps the state. The playing track's file is the scan's, so it
  waits too. The walk, compactions, the save, covers and the build go on:
  they are short, a cut is safe (N4), and they are what the listener sees.
- **The build right after a walk (U11):** `buildAfterWalk(added, toScan,
  msPerFile)` is true for 200 added files or more, or a scan estimated over
  60 s; else the update step waits for the scan's end. It is asked only
  after a walk that found changes. The estimate takes the scan's own mean
  step once it has one (`stepDone()`), else 18 ms a file (3.3.4's 3-6 min
  for 20k while playing, with the sector cache).
- **Where the time went:** the time spent waiting on each reason, and each
  job's steps, mean and longest step (`stepDone()`): `gs` and L3's figures.
- **millis()'s wrap:** a window is cleared once it has passed, so an old
  one can't come back 24.8 days later (a device on USB for weeks).
- **The glue's inputs (N10):** `listMoving` is the page's `animating()`;
  `input` is `stepIdle()`'s; the ring and the underruns are the backend's
  `bufferedMsNow()` and `underrunsNow()`; `decodePassUs` needs a peak the
  decode task raises after each pass and the loop takes (swapped to 0);
  `decoderAtEnd` and `trackSeq` are the backend's join state and heard
  token; `btSetup` is `pairingUnderWay()` or a page burst (BtLink Paging),
  `btSeq` counts the link's ups and downs; `battery` is -1 until read
  (main.cpp's snapshot clamps a failed read to 0 today, which would hold
  the scan).
- **`LibraryWrite`** (after `QueueWrite`, before `Busy`) holds for the
  whole update step, from the queue's flush (3.4.2, step 1) to the end of
  `library.idx`'s save, and for a whole compaction, not only for their
  renames (3.3.5): the step gives the queue's memory to the build
  (`QueueModel::release()`), so the idle power-off's `flushNow()` in the
  middle would write the queue empty, and both are seconds of work. The
  sleep timer turns the power off only through the idle countdown (it
  pauses, and the countdown runs from the pause), so the blocker covers it
  too. As built (N10), `stepIdle()` feeds it `CardTasks::libraryWrite()`:
  true for every compaction step and, since N12, from the update step's
  fence (the queue's flush) to its save's end on the card worker (3.9).
  (2026-10-08 review: this said main.cpp didn't feed it yet.)
- **Proven on the host:** `test_scan_scheduler` holds each yield for
  exactly its window after its cause, in its order; the battery's
  hysteresis and the jobs it holds; covers first and the build before
  everything; one step at a time and each step's priority; the jobs' and
  the sources' order; a 60-day run across millis()'s wrap; and a random
  session of 900,000 passes (about 5.6 simulated hours, across the wrap)
  against a model of the rules written apart from the code (each cause as
  the last time it was seen, in 64-bit time): every answer equal. Six
  mutations of the rules (an off-by-one window, two yields swapped, the
  floor's resume at 15%, the DJNB check unfloored, covers during a moving
  list, no baseline for the counters) each fail it. `test_idle_policy`
  adds `LibraryWrite` to every blocker's test and to the random run, and
  runs the real `SleepTimer` into `IdlePolicy`: an update step at the end
  of the countdown after the timer's pause holds the power-off until 20
  min after it ends, an update step in the warning ends it, and one in
  the release cancels it.

### 3.4 The builder

#### 3.4.1 Inputs and the merge

- **Two streams in canonical order:** T (valid per 2.5) and D (always
  compacted before a build). The build is a 2-way streaming merge with
  two 8 KB PSRAM buffers and no hash map of paths.
- **Checked while streaming.** T's section CRCs and 2.4.3's order checks
  are computed as the merge reads; a failure is known only at the end
  (or wherever the order breaks), so it **restarts the build from D
  alone**, T absent, before anything is shown. D, the device's own file,
  gets the same checks; a bad D is dropped and rebuilt by the walk and
  the scan.
- **The listing** (which files exist) is D's entries. When D's walk
  identity is older than the root's (no walk since the commit), T's paths
  count as present too: section 2.9's "before the first walk". Then D's
  Software rows whose paths the new T no longer lists are dropped (the
  commit deleted those files or gave them up; the walk re-adds any still
  on the card), so no ghost tracks are browsable for the 10-20 s before
  the walk. A T path a walk at this commit didn't see is left out.
- **Per path,** section 2.9's rules: T if fresh, else D if its size and
  time are equal, else the path. The chosen record's `known` fields are
  used; any other field comes from the path parse (`readNames()` in `LibraryIndex.cpp`).
- **Per album and per artist,** part 5's election rules.

#### 3.4.2 The update step (the only rebuild path; also the boot's)

**Preconditions:**

- the journals compacted;
- a safe point: nothing plays, or the playing track has at least 20 s
  left and there was no seek in the last 2 s (so the decoder can't reach
  its end of file and ask for the next path while the index is down);
- a memory check: free PSRAM plus what the step frees is at least 1.1 ×
  the estimated build peak, and the largest free block
  (`heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)`) at least the
  arena's estimate. Otherwise a toast says "Library updates at next
  boot", the step writes the **build-at-boot marker**
  `/.player/build.req` (an empty file), and the next boot builds before
  the UI, on a fresh heap (3.2.2's first row). Without the marker that
  boot would load the old index, whose hard inputs still match, and meet
  the same memory check again, every time.

**Who runs what.** `LibraryIndex` is loop-task only, and a build of
several seconds on the loop would freeze touch, the redraw,
`bt_.update()`, the queue saver and `IdlePolicy`. So:

- steps 1-3 and 5 run on the loop;
- step 4, the build, is a job on the card worker, at priority 1
  (3.3.4). The worker runs one job at a time, so the scan, the
  compaction and the walk are paused until the save (step 6) is done:
  nothing writes or removes `tags.bin` or the journals while the build
  streams them (`FF_FS_LOCK` is 0 in this build, so FatFs wouldn't
  refuse an `f_unlink` of an open file);
- the "Updating library" state of step 2 is the fence: while it holds,
  nothing on the loop reads `LibraryIndex` or `TrackCatalog`;
- a portable `LibraryUpdate` state machine (N12) owns the sequence, the
  safe point, the memory check and the marker, host-tested like
  `test_idle_policy` (as built: 3.9).

**The sequence:**

1. `queueStore.flushNow()`: `queue.txt` now *is* the queue.
2. The UI enters "Updating library": Now Playing keeps a copy of the
   playing track's title, artist and album; the lists show the status
   line; queue edits and seeks wait; the decoder keeps its open file.
3. Free Thumbs' pools (about 315 KB; `libraryChanged()` clears them
   anyway), AutoDJ's maps, the queue's entries and undo snapshot (a new
   `QueueModel::release()`), and **the old index** (`LibraryIndex::clear()`).
4. Build, on the card worker: `begin(Sizing)` from the headers' counts
   (exact: no doubling, no slack), the merge feeding `addRecord()` and
   `addFile()`, then `finish()` (sort, votes, views, trim). A bad CRC or
   order in T restarts it from D alone (3.4.1).
5. Live again, on the loop: re-read `queue.txt` with `restore()`'s logic,
   carrying the resume start point as `remap()` does today, its sinks
   pre-sized from the file's entry count; lengths from the index;
   AutoDJ's join (3.5), reading DJRW and DJPH and checking their CRCs as
   it reads (0.7 MB); Thumbs' pools back; `userInterface->libraryChanged()`.
6. Save `library.idx` on the card worker, aside then renamed, under the
   `LibraryWrite` blocker; then remove the marker if there was one. The
   index isn't changed while it is written. Then, once per commit, the
   worker checks DJNB's CRC (6 MB, 3.5-5 s, priority 0); until it passes,
   AutoDJ uses random with filters (2.13.4), and a failure makes the
   MPDJ absent.

**Time (ESTIMATED from MEASURED rates):** docs/UI-SPIKE.md measured the
index build on the device at 10,000 synthetic tracks: adding them
1.1-1.3 s idle and 2.0 s while an MP3 plays, finishing (sorts, views,
trim) 0.6 s idle and 1.0 s during MP3. Scaling the adds linearly and the
finish as n log n, at 20k:

| Part of the pause (steps 2-5) | Idle | While an MP3 plays |
|---|---|---|
| Reads: T's and D's sections, about 6 MB at 1.2-1.7 MB/s | 3.5-5 s | 3.5-5 s |
| Adds | 2.2-2.6 s | about 4 s |
| Finish | about 1.3 s | about 2.1 s |
| The queue's re-read, 20k lines (3.2.5; at most 5,000 since the cap: 0.4-0.5 s) | 1.5-2 s | 1.5-2 s |
| AutoDJ's join | 0.3-0.5 s | 0.3-0.5 s |
| **The pause** | **about 9-12 s** | **about 11-14 s** |

The card reads busy-wait the CPU (3.3.4), so they don't overlap the
adds. A short queue takes 1.5-2 s off; a full one since the queue's cap
(5,000 lines, 3.5) about 1-1.5 s, which the table keeps as margin until
L4 measures. At 2k the pause is about 1-1.5 s.
The save follows in the background (about 1.8 MB at 0.5-1 MB/s, 2-4 s).
The safe point's 20 s still covers the worst case, with about 6 s to
spare (the decoder asks for the next path only near its end of file); if
L4 measures a pause over about 16 s, the safe point grows with it.

**Callers:** the end of a scan; a walk that found changes; `g0` and `gb`;
the UI's "Try again"; the boot (3.2.2). Today's `rebuildLibrary()` in
`main.cpp` becomes this step, and `QueueStore::remap()` goes through
`queue.txt` instead of its in-PSRAM text copy (a doubling `MemorySink`),
which fails at about 15k entries (metascan section 6.2).

#### 3.4.3 `library.idx` v6

| Record | Size | Fields |
|---|---|---|
| **Track** | 32 B (was 24) | `name`, `title` (a slice of the name when the tag title is a substring of it, about 88% of files; else an interned string), `folder`, `album`, `artist` (the folder artist, unchanged for Go to and `notePlaying`), `trackArtist` (the display join, kNone when it equals the album's line), `titleLen u8`, `format u8`, `disc u8`, `flags u8` (the record's source, a JPEG picture, compilation, pending), `number u16`, `durationS u16` |
| **Album** | 28 B (was 20) | `name` (the display name), `artist`, `folder`, `firstTrack`, `trackCount`, `artistLine` (a string), `year u16`, `discs u8`, `flags u8` (`kLoose`, a transfer thumbnail) |
| **Artist** | 20 B, unchanged | `name` is the display name |
| **Folder** | 36 B, unchanged | |

- **The header:** magic MPLI, `kVersion` 6 (5 today), the record sizes,
  `rulesVersion`, the inputs (hard: the transfer's identity and whether
  T was present; soft: D's headerCrc and journal sequence), a
  `buildStamp`, the counts, the A-Z buckets, the FNV trailer as today.
- **The loose-tracks album** keeps the name "" and gets `kLoose`:
  `SleepTimer`'s end-of-album rule and the "(loose tracks)" rows test
  that "" today, and switch to the flag.
- **`load()`:** Loaded (compare the inputs: a hard mismatch, or the
  build-at-boot marker (3.4.2), rebuilds from the records at boot; a soft
  one keeps the index and rebuilds at the
  scan's end); Outdated (versions 1-5, or an older `rulesVersion`: build
  from the records, else walk); Corrupt (an older firmware sees v6 this
  way: it rebuilds its own v5 from a walk and ignores `/.mstream` and
  D; this firmware then treats that v5 as Outdated: one rebuild each
  way, nothing lost); NoMemory as today.
- **The views** keep each artist's and each album's sort key (the string
  the A-Z lists sort it by: its elected sort tag, else its name), so a
  loaded index can give the rail, a row's letter and the jump grid the key
  the order and the buckets use (`artistSortKey()`, `albumSortKey()`).
- **Size at 20k (ESTIMATED):** tracks 640 KB, albums (about 1.8k) 50 KB,
  artists 14 KB, folders (about 2.6k) 94 KB, views about 200 KB (the sort
  keys about 10 KB of it), strings
  about 0.8 MB (today's names about 0.68 MB plus about 0.12 MB of tag
  strings): **about 1.75-1.85 MB, 88-92 B per track** (75-79 B today,
  MEASURED).

#### 3.4.4 As built (N2)

What the code decided where the design left room, and what it measured on
the host:

- **The votes count as the records arrive**, over each album's and each
  artist's run of tracks (the builder adds files in canonical order, so a
  folder's tree is one run), in a fixed scratch from the index's hooks
  (about 42 KB: 1,024 strings, 64 years, 512 artist candidates), not in
  `buildViews()`: keeping every track's tag strings until `finish()` would
  cost 0.4-1 MB at 20k. A record added out of that order still indexes; its
  album takes the names its last run elected.
- **The strings live in 64 KB chunks** (a loaded index: one block the
  chunks point into), so their size needs no estimate, no doubling and no
  copy. The record tables are sized from the headers' counts (upper
  bounds) and trimmed by `finish()`, in place when the allocator gives a
  shrink hook (`heap_caps_realloc` on the firmware: N10's glue), else by a
  copy as before.
- **Stage A's order inside an album applies to an album with a record**
  (the album flag `kTagged`); an album of path names alone keeps today's
  order (folder, disc, number with none first, name). So a card with no
  records indexes exactly as today, to the byte apart from the version
  and the record sizes: `test_library_index`'s path tests pass unchanged.
  Track flags also say which of the title, the number and the disc are the
  record's (`kTagTitle`, `kTagNumber`, `kTagDisc`).
- **`load()` keeps `Stale`** for a hard-input mismatch, and the walk
  signature is one of the hard inputs (N10's boot stopped computing it on
  the card: a saved one doesn't match, 3.8).
- **Sort tags:** an album's `albumSort` comes with its winning album
  value, an artist's with its winning display (`albumArtistSort` for an
  album-artist display, else `artistSort`); the A-Z rails follow the sort
  keys, which the views keep (3.4.3), so `LibraryPage`'s rail, its rows'
  letters and the jump grid key on them too. A sort tag is trimmed of
  White_Space, and one with nothing left (a lone tab, which 2.3.6 stores
  as a space) is no sort tag, as 5.4's orderName says.
- **The builder's inputs from N4 and N5** are interfaces: D's statuses
  (DSTA) through `LibraryBuilder::DeviceRows`, the folders' cover facts
  (DFLD) through `FolderFactsSource`; with no statuses every D record is a
  full one. T's THUMB folders are collected from its folder steps (8 bytes
  each).
- **Sizing when T lists** (no walk since its commit: the boot after a
  transfer): D's Software rows are T's files (counted in T's header) or are
  dropped, so the index is sized from T's counts plus D's rows that aren't
  Software and the folders on their paths, which DSTA's header carries
  (`DeviceRows::ownCounts()`, N4). Without them every D row counts too,
  and at 20k right after a transfer (a Software row for each file) every
  block would be twice as big: a peak of 2.90 MB instead of 1.99 MB, and a
  1.28 MB track table where 3.5 counts 640 KB.
- **nameKey's tables are Rust's own** (`tools/unicode_case.rs`, Unicode
  17.0 from rustc 1.98), checked against std code point by code point
  (`test_name_key`).
- **U8 (decided: the default stands) and U9 (b)** keep the proposed
  defaults behind `LibraryBuilder::kTransferBeatsRescan` and
  `LibraryIndex::kHandCoverBeatsThumbnail`.
- **Measured (host):** the tagged synthetic library (`LibrarySynth`) in
  the user's shape, with the measured disagreement rates and name lengths,
  at 20k: **1.78 MB, 89.1 B a track** (strings 0.78 MB, views 0.20 MB with
  the sort keys); **a build peak of 1.99 MB**, the builder's own 58 KB
  included, walked or with T listing after a transfer. Built from T and
  from D, the same files give the same bytes once the tracks' sources are
  cleared, when the two records' lengths are equal; with the device's
  50 ms shorter (2.17 item 3's encoder delay), 133 of 3,000 tracks are a
  second shorter, and the rest is the same bytes.

### 3.5 Budgets

**PSRAM at 20k tracks, with AutoDJ loaded** (ESTIMATED from MEASURED
counts):

| Item | MB | Basis |
|---|---|---|
| Free with the UI up, today (77-track card) | **2.77-2.90** | MEASURED (dev.log `psram=`) |
| `library.idx` v6 | −1.75 to −1.85 | 3.4.3 |
| The queue, full (the cap: 5,000 entries), with its undo snapshot | −0.12 | 12 B per entry, twice (`QueueModel`). With no saved queue the boot queues the library's first 5,000 (`queueEverything`), so a full queue is the *default* on a big new card; `assign()` takes no snapshot (0.06 MB), and the first edit after it adds one. A whole-library queue of 20k was −0.48 (−0.96 untrimmed) before the cap |
| AutoDJ: u16 maps, filters, 5 cached rows | −0.17 to −0.33 | autodj research section 5 |
| The sector cache | −0.13 | 3.2.4: 256 sectors, 135,168 B (3.2.7: 0.06 MB more than the 128 first planned) |
| `DurationBook` | −0.04, or 0 once lengths come from the index | `lib/core/QueueView.h` |
| The card worker's job, only while one runs (one at a time) | −0.08 to −0.1 | the largest job: the walk (the 64 KB scratch, DFLD's 8 KB buffer, the 2 KB Δ histogram, the journal's write buffer). The scan: its 4 KB buffer, the record, the `FIL`, a 100-record chunk, the resume set (about 60 KB). The compaction: its run buffers (about 60 KB). The doubtful files go to `walk.jnl`, not RAM (3.2.3) |
| **Headroom** | **about 0.20 to 0.65** | positive in the worst case since the queue's cap (it was about −0.16 to 0.29 with a whole-library queue); the sector cache's 256 sectors took 0.06 MB of it (3.2.7) |

**What that forces:**

- **Covers stream.** Thumbs reads a whole JPEG into PSRAM (capped at
  2 MB), which doesn't fit at 20k. TJpgDec's input is fed from the file
  in 4-16 KB chunks instead, which also lifts the 2 MB cap. The scaler's
  about 170 KB during a decode fits only in the better half of the range,
  so transfer thumbnails (one 3.2 KB or 18 KB read, no decode) matter at
  20k.
- **The queue is capped at 5,000 entries** (the user's answer to U12,
  2026-10-07; docs/QUEUE-MODES.md section 15): Shuffle all and Play all
  on a bigger library take a random 5,000 (shuffle on) or the first 5,000
  (off); an add takes as many as fit and is refused with "The queue
  holds 5,000 tracks" when none do; the undo is always kept. A longer
  `queue.txt` from before the cap loads its first 5,000 lines, or the
  5,000 from its current line on, and is written again. Its blocks are
  trimmed to their exact size after `assign()` and never grow past the
  cap.
- **During the update step:** it frees the index, the queue, AutoDJ's
  maps and Thumbs' pools (about 2.7-3.4 MB with the headroom); the build
  peak, pre-sized, is about 2.0 MB; margin about 0.7 MB or more.
  Afterwards, in order: AutoDJ's join (a sorted array of (path hash,
  track id), 240 KB transient, then 2 × 40 KB of u16 maps; rows whose
  file is gone are skipped), the queue's re-read (at most 20 KB
  transient, 40 KB shuffled, then 60 KB), Thumbs' pools (315 KB).
- **Fragmentation:** the arena (about 0.8 MB) and the track table (640 KB)
  are single blocks; the memory check (3.4.2) defers a build to the next
  boot rather than fail, and the build-at-boot marker makes sure that
  boot builds. (As built, 3.8: a build takes its strings in 64 KB chunks,
  and the index keeps its track table's block across the update step's
  rebuild, taken again when the new table fits, so only a load at boot
  asks for the arena whole, on a fresh heap.)
- **Nothing is read whole** into PSRAM by the walk, the scan or the
  compaction: their temporaries are fixed buffers (above), whatever the
  card holds.
- **AutoDJ's filter data** is the DJRW rows' (bpm10, camelot) only;
  random with filters is narrowed to it (2.13.4). A per-track filter
  block for every file would cost about 80 KB more (part 7, U15).

**Levers, if L0 or L4 measure less** (part 7, U12, which the cap
answered):

| Lever | Saving |
|---|---|
| ~~No undo snapshot for queues over about 5,000 entries~~ | taken another way: the cap saves 0.36 MB against a whole-library queue of 20k and keeps the undo |
| File names kept on the card instead of in PSRAM (about 31 B per track) | 0.62 MB |
| `DurationBook` folded into the index | 0.04 MB |
| AutoDJ at its compact layout | up to 0.16 MB |

The ceiling was about 25k tracks with a full queue (metascan section
6.2). Since the cap the queue no longer grows with the library; the
index, AutoDJ's maps and `DurationBook` do (about 100 B a track): about
23k-27k tracks by the table above (ESTIMATED), before the levers.

**Flash, IRAM, internal RAM** (metascan section 6.1): TagScan about 11.4
KB of code and 4.0 KB of rodata (MEASURED with the ESP32 toolchain), 0
IRAM, about 1 KB of stack per parse; the scan engine, the contract kit,
the builder and the UI about 10-20 KB (ESTIMATED); the sector-cache
wrapper 1-2 KB. The app is 2.3 MB in a 6.3 MB slot: flash is no
constraint. IRAM is untouched, and the `cache_guard` build check still
applies to any layout shift. (MEASURED at N10, 3.8: the glue and every
part it links added 76.2 KB of code and 16.7 KB of rodata, 472 B of
internal DRAM and no IRAM; the app is 2.48 MB, 41% of its slot.)

**Card time** at 20k (ESTIMATED): reading T's FOLD, RECS and STRS, about
3.9 MB, takes 2.3-3.3 s, only during a build; checking DJNB's CRC, 6 MB,
takes 3.5-5 s once per commit in the background; an AutoDJ pick reads
one 300 B row (about 3 ms).

### 3.6 What changes, file by file

| File | Change |
|---|---|
| New `lib/core/CardContract` (with `CardContainer`, `CardTags`, `CardManifest`, `CardAutoDj`; built in N1) | The contract kit: CRC-32, FNV-1a 64, qfp, FAT time and the skew rule; MSMF, MPTG, MPDJ and MSPD readers and writers (the device writes only MPTG; the writers serve the host tests and the future sync agent); the root election; `device.txt`; the canonical order; 2.4.3's structural checks. Its golden files are 2.17's, its vectors 2.18's. |
| New `lib/core` modules | `TagStore` (D, `tags.jnl`, `walk.jnl`, the streaming compaction, recovery, the cut-rename rule of 2.12.6); `CardWalk` (an `IDirLister`, the canonical sort with its passes, the digests, T's freshness, the skew, the merge); `SectorCache`; `TagScan` (the production port of the prototype, with part 5's rules); `ScanScheduler`; `LibraryBuilder` (the merge into `LibraryIndex`, part 5's votes, the streamed checks); `LibraryUpdate` (the boot decision and the update step as a state machine: built in N12, 3.9). Built in N10 (3.8): `LibraryBoot` (3.2.2's table), `CardRoot` (the root as the boot reads it) and `CardJobs` (the walk, the compaction and the scan, a step at a time). |
| `lib/core/LibraryIndex.{h,cpp}` | v6 records and the header's inputs; `begin(const Sizing&)` with exact counts; `addRecord(path, const TagView&)` next to `addFile()`; Stage A's votes and orders in `buildViews()` (a missing number sorts last, an artist's albums newest first); `readNames()` fills only the fields a record lacks; `kLoose` and the transfer-thumbnail flag; library roots (LIBR), if the vpath layout is chosen. `Load::Stale` no longer happens at boot. |
| `lib/core/TrackCatalog.{h,cpp}` | `title()` the tag's own string or the slice; `artist()` the track artist, else the album's line, else the folder artist; `album()` the display name; `durationHintMs()` the library's length; a one-slot overlay for the playing track's fresh tags (3.3.3). Built (N9, 3.7), with `albumArtist()` and `year()`; new `lib/core/LibraryText` holds the rows' texts. |
| `src/app/Library.{h,cpp}` | The boot decision (3.2.2) replaces `begin()`'s walk, `library.tmp` recovery and the build-at-boot marker included; `rebuild()` becomes the update step, driven by `LibraryUpdate`, its build a card-worker job; the save moves to the card worker; `report()` gains the scan state. Built (N10, 3.8): the boot, the build from the records, the save through 2.12.6's rule, `device.txt`, the overlay, the memory check and the marker; `rebuild()` builds from the records on the loop (the card worker's build and save are N12's). New `src/app/CardWorker` (the task) and `src/app/CardTasks` (its loop side). Built (N12, 3.9): the boot and the update step are `LibraryUpdate`'s, its build and its save card-worker steps; `index()` is nullptr behind the fence, the catalog answers Now Playing from a held copy; `rebuild()` is the flash's alone. |
| `src/app/QueueStore.cpp`, `lib/core/QueueModel` | `remap()` through `queue.txt` (flush, free, rebuild, re-read); the reads pre-size their sinks; `QueueModel::release()` and an exact-size trim. Built (N3). Then the cap of 5,000 entries (`kMaxEntries`, `room()`, `window()`; `queuetext::read()`'s window; the UI's toasts): built, QUEUE-MODES.md section 15. The remap in two halves around a build on the card worker (`queueremap::Carry`, `remapBegin()`/`remapFinish()`, the player fenced between): built (N12, 3.9). |
| `src/storage/LocalStorage.cpp` | A FatFs lister (`FILINFO`'s size and time); the sector-cache wrapper after `SD.begin()`; `forEachFile` stays for LittleFS and the console. Built (N10, 3.8): new `src/storage/CardFat` (the lister, N4's file interface over FatFs) and `src/storage/SectorDisk` (the wrapper), installed after the mount. |
| `src/ui/Thumbs.{h,cpp}` | The worker becomes the shared card worker (walk, scan and cover jobs); cover sources in 2.14.3's order, `/.mstream/thumbs` read-only (and `hasCover()` true for an album with the transfer-thumbnail flag); streamed JPEG input. Built (N10, 3.8). |
| `src/ui/LibraryPage.cpp`, `NowPlayingPage.cpp` | Direct `trackTitle()` reads move to the catalog; rows show a year subtitle, disc dividers and a track-artist subtitle; the status line. Go to artist and album keep the folder entities. Built (N9, 3.7); the status line's drawing too (N10, 3.8: `ui/Ui`, at the bottom of a Library page). |
| `lib/core/SleepTimer.cpp` | End of album tests `kLoose`, not an empty album name. Built (N9). |
| `src/spike/Spike.cpp`, new `src/app/TagConsole`, new `lib/core/TagText` | The console's `g` commands (3.3.6). Built (N9, 3.7); the card worker's jobs (`gr`, `gw`, `gb`, `gv`) are filled (N10, 3.8), with `gc` and `gl` (6.3.1). |
| `lib/core/IdlePolicy.{h,cpp}`, `src/main.cpp` | The `LibraryWrite` blocker; the journal flush at shutdown; the boot and `rebuildLibrary()`. Built (N7, N10): `LibraryWrite` during a compaction and the update step, the scan's chunk written at the idle power-off and the CPU speed's restart, the update step run when the card worker's jobs ask (3.8). |
| `lib/core/NvsLayout.h` | **No change** (`kCurrent` stays 2): the library's state lives on the card and moves with it. AutoDJ's on/off would be schema 3 (autodj research section 7), outside this plan. |
| `lib/core/OpusOpenCache` | **Unchanged:** keyed by path hash and size and checked at the open, so rebuilds don't touch it. |

### 3.7 As built (N9): the catalog, the UI and the texts

What the screens say comes from N2's index through `TrackCatalog` (a
track id) and `lib/core/LibraryText` (an artist, an album, a row), both
host-tested in `test_ui_library`; the firmware builds them and nothing is
flashed. Until N10 built the index from records, the firmware's index was
the walk's (paths alone) and every name read as before. What the code
decided where 3.6 and 5.4 left room:

- **An album's artist line** (`librarytext::albumArtist()`): the line its
  records elected when it has any (`kTagged`), else its artist's name. An
  album with no record keeps the artist folder's name as its stored line,
  while the artist itself may show the spelling its other albums' tags
  elected (the folder "Lantern Choir" shown "The Lantern Choir"): so an
  untagged album says what its artist row says. `TrackCatalog::artist()` is the
  track's own display, else that line; `albumArtist()` and `year()` are
  new; `durationHintMs()` gives the record's length in whole seconds (the
  index keeps no more), which PlaybackController hands the backend and
  the bar until the open measures it. A path-only track's hint stays 0,
  as before.
- **The overlay** (3.3.3) is `TrackCatalog::Overlay`, about 800 bytes its
  owner holds in PSRAM: one track's title, artist display (the list
  joined by 5.4's rule), album, year and length from a record as the
  builder reads it (`LibraryBuilder::viewOf()`), each cut to 255 bytes. A
  field the record lacks stays the index's. It is keyed by the track id
  *and* the index's `buildStamp()`, so a rebuild that renumbers the tracks
  ignores it without anyone clearing it; an artist folder's loose tracks
  keep no album and no year whatever it says (the next build would show
  them so). `namesVersion()` changes with every set and clear, and Now
  Playing draws its names again when it does. The scan's glue sets it for
  the playing track (N10, 3.8).
- **The rows** (`LibraryPage`, its texts in `librarytext`): an artist,
  "3 albums, 41 tracks" ("1 album, 1 track": the counts are singular when
  they are 1 now); the root's Albums, the album's line and year
  ("The Lantern Choir · 2001"); an artist's albums, newest first (N2's
  view), "2001 · 14 tracks"; an album's header, "Artist · 2001 · 14
  tracks"; a track, its title through the catalog (the overlay's for the
  playing track) and, under it, its own artist where it differs from the
  album's line (the builder stores none where it is the same), with its
  album after it on an artist's All tracks ("Guest · Album"). A cut text
  never ends mid-character. The A-Z rail, a row's letter and the jump
  grid key on `librarytext::railName()`, the sort key's sort name, which
  matches the index's buckets row for row on the tagged synthetic library
  (a test). The Folders view stays raw.
- **Disc dividers** (`librarytext::Discs`, the Album page's rows): an
  album whose highest disc is over 1 gets a "Disc N" row (Bold, the
  Library's accent, a hairline under it) before each disc's first track,
  "Disc 1" included; a divider is no control (no bar, no sheet, never
  tinted, no highlight under the finger and no tick: the list's
  `Source::pressable()`; 2026-10-08 review: a touch lit it and a tap
  ticked), and a track's number falls back to its place in the album, not
  its row. With records the tracks sort by disc first, so each disc is one
  run. A path-only album whose names give discs ("1-01", "2-01") gets them
  too: a visible change of this firmware on today's cards. One that sorts
  folder by folder with its discs interleaved (each subfolder a disc 1 and
  a disc 2) has a divider at each change, up to 64; past that, none at
  all. The page reads the album once when it opens (64 starts, about 400
  bytes in the page).
- **Now Playing**: the artist row is the catalog's (the track's), and
  "Unknown artist" (`uitext::kUnknownArtist`) when neither a tag nor a
  folder names one: the placeholder reworded, since a tagged track at the
  top of /music has a name now. The album row adds the year,
  "Album · 2001", when the whole fits its 190 px, else shows the album
  alone. The navigation menu's details name what Go to opens, the folder
  entities (the artist's and the album's shown names), no longer the
  catalog's, which are the track's own now.
- **The texts** (`UiText`, each width-tested with the firmware's fonts):
  the no-music state ("Put your albums in /music/Artist/Album/ (MP3, FLAC
  or Opus), then tap Try again.") and the no-card lines drop the file-name
  pattern ("01 - Title.mp3": tags name the tracks, and a name only where
  a file has none) and keep the folders, which still make the artists and
  the albums in Stage A; "Unknown artist"; "Disc %u"; and 3.3.6's status
  line and toasts ("Checking the card…", "Reading tags 1,234 / 19,410",
  "Updating library…", "The last transfer didn't finish", "Found 12 new
  tracks", "Library updated", "Library updates at next boot") with
  `librarytext::statusText()` and `foundText()`. The status line isn't
  drawn yet: nothing has a scan state to show until N10 and N12, which
  draw it across the list's width (304 px, Small, the room the test
  checks) and fire the toasts. The Output tab's Library row and its
  Rescan tags button (3.3.6) wait for the same. (N10 draws the line and
  fires the toasts, 3.8; N12 the Output tab's row, 3.9.)
- **SleepTimer**: End of album tests the album's `kLoose` flag, not its
  name: two album folders that share a tag name are two albums, one
  folder whose tracks disagree is one, and an artist folder's tagged
  loose tracks are still no album (`test_sleep_timer`).
- **The console** (3.3.6; `tagtext::parse()` for the argument,
  `app/TagConsole` on the loop task, about 30 KB of PSRAM while a command
  runs): `g` adds a line on where the names came from ("19,410 tracks:
  19,000 from the transfer's records, 400 from the device's, 10 by their
  paths (10 for the scan)") and the scan's state; `gs` reads D (its
  header, DHDR, its rows by status through DSTA), the journals' sizes, the
  root election and T's header (and whether it is the companion the root
  names), and `pending.bin`; `gt</music/...>` reads the file with TagScan
  now (its result, reads, bytes and time), D's and T's records of it, each
  file checked whole first (`mptg::check()`, seconds at 20k), the record a
  build would take (`LibraryBuilder::choose()` with D's row as the
  listing, DHDR's skew, and T counted as listing when D's walk wasn't at
  the root's commit), and the index's names for it. The file's FAT time is
  its `getLastWrite()` turned back by `gmtime()` (the VFS made it with
  `mktime()` and no time zone, which the firmware never sets).
  `gr`, `gr!`, `gw`, `gb` and `gv` are the card worker's jobs:
  `TagConsole::Jobs` hooks (and a `state` hook for the scan's line) that
  N10 and N12 fill through `Spike::tags()`; until then each says there is
  no worker and changes nothing. `g?` (anything else) lists them.
- **Proven on the host:** the rows and headers of a tagged library and of
  a path-only one (`test_library_rows_from_tags`, `_from_paths`), the
  dividers (`test_disc_dividers`), the catalog and its overlay
  (`test_catalog_names_and_overlay`), the rail on sort keys
  (`test_rail_follows_the_sort_keys`, 3,000 tagged synthetic tracks), the
  texts' widths (`test_library_texts_fit`), the console's parse, dumps
  (crafted and the corpus's `flac_full.flac` through TagScan) and sources
  (`test_console_*`), and End of album with tags
  (`test_album_ends_between_with_tags`).

### 3.8 As built (N10): the firmware glue

N1-N9's parts run on the device now: the boot of 3.2.2, the validation
walk, the compactions and the scan on one card worker under N7's
scheduler, the sector cache under FatFs, the transfer's thumbnails and
streamed covers, `device.txt`. Built for `core2` and `core2-dio` with every
guard, **not flashed**: nothing here has run on a Core2 yet (6.3's batch
measures it). The portable parts are host-tested in `test_card_jobs`
(lib/core `LibraryBoot`, `CardRoot`, `CardJobs`); the rest is glue in
src/. What the code decided where 3.2-3.4 left room:

- **The files.** `storage/CardFat` (FatFs itself: N4's `tagstore::Fs`
  and N5's lister, `cardjobs::Card`), `storage/SectorDisk` (the diskio
  wrapper), `app/CardWorker` (the task), `app/CardTasks` (its loop side:
  the scheduler, the steps, what follows them), `app/Library` (the boot,
  the build, the save), `ui/Thumbs` (covers as worker steps), and in
  lib/core `LibraryBoot` (3.2.2's table), `CardRoot` (the root as the boot
  reads it) and `CardJobs` (the walk, the compaction and the scan, a step
  at a time).
- **FatFs, not the VFS.** The SD library mounts its physical drive as
  FatFs's logical drive of the same number; the boot reads it from
  `SDFS::_pdrv` (protected: through a member pointer) and every path is
  that drive's (`"0:/music/Artist"`, `"0:/.player/tags.bin"`). A `FIL`
  (about 4.1 KB here), `FF_DIR` and `FILINFO` are PSRAM objects; the
  lister's are allocated once (3.3.1's one `FIL` per worker), `Fs::open()`
  makes one per open file. Reads and writes go to FatFs in pieces of 4 KB
  at most with a yield between, as `FileStream` does, so the decoder's
  reads of the playing file (the same volume lock) never wait long.
  `firstCluster()` is the open `FIL`'s `obj.sclust`; `rename()` is
  `f_rename`, which refuses an existing target, as N4 asks. The decoder,
  the queue and the thumbnails keep the VFS; both reach the same FatFs
  volume under its lock.
- **The boot** (`Library::begin()` on the card; the flash keeps today's
  signature walk): `cardroot::read()` (the election of 2.5.4, each root
  checked whole, then T's frame and header against its COMP entry: three
  small files read; LIBR's roots, at most 16, kept), `TagStore::open()`
  (its recovery), 2.12.6's `settle()` of `library.tmp` (whole when its
  header reads and its sum holds: it is loaded once to know), the marker,
  then `LibraryIndex::peek()` of `library.idx`'s header (a new portable
  call: the header alone, 312 bytes) and `libraryboot::decide()`.
  **Whether T was used isn't a hard input the boot compares**
  (`libraryboot::matches()`): a T whose sections fail at the build has the
  same identity at every boot and fails again, so asking "was T used" of
  the saved index would rebuild it at every boot; the load takes the saved
  inputs as they are. A saved walk signature (any index an older firmware
  saved) doesn't match: the first boot of this firmware builds (or walks)
  once. A load whose sum fails falls to the same table as unreadable.
  Each boot logs one line for the card ("T tags-0000002a.bin, generation
  42 ...; D present (N records, M journal chunks)") and one for the
  decision ("library.idx matches the card: load (its inputs are the
  card's) in N ms"), then "browsable N ms after the mount".
- **`device.txt`** is formatted from `devicetxt::current()` with the
  version less its "v" (`firmware=0.7.0-17-gcc3413b`), compared with the
  file byte for byte, and written through `device.tmp` and `replace()`
  only when it differs: a new firmware rewrites it once.
- **The build** (`Library::buildCard()`): the journals compacted first,
  then `LibraryBuilder` over T (when the root has one and this session
  hasn't found it bad) and D, opened as three files (the merge, DSTA's
  rows, DFLD's facts: each read front to back), D's skew only when its
  walk was at this commit, T listing (`transferLists`) until that walk,
  LIBR's roots, the index's trims in place (`heap_caps_realloc` as N2's
  shrink hook). No records at all: /music walked as before (the VFS
  walk), saved with the root's identity, every track Pending (the files
  are all unread: the scan's sources take the playing track, the queue
  and the Library tab's page first, and the status line counts them;
  2026-10-08 review: a walked index had none Pending, so a hand-filled
  card's first session read D's order alone and counted "N / N"); the
  decision line's time is the walk's. A T the boot's build left out (its
  stream failed a check) is bad for the session: the jobs leave it out
  too. A build whose compaction failed (a full card, a walk being written)
  reads `tags.bin` alone and saves `libraryboot::kJournalsLeftOut` as its
  journal sequence, which no store has: the next boot finds the index
  soft-stale and rebuilds it at its scan's end (review: it saved the
  journal's own sequence, claiming records it never read, so they stayed
  out of the index until something else asked for a build).
- **The sector cache's wrapper** (`storage/SectorDisk`, 3.2.4 and 3.2.7;
  its rules are lib/core `CachedDrive`, host-tested in `test_sector_cache`
  and under FatFs in `test_fat_model`): installed right after `SD.begin()`
  (the stock driver registered, the cache cleared, then
  `ff_diskio_register()` with the wrapper), before the audio starts;
  `probeCard()` mounts only to look and gets none. **FatFs's own remount
  clears it too:** with no card-detect, a card pulled or swapped while
  the player is on makes the SD driver's status say `STA_NOINIT`, and
  FatFs's next call mounts the volume again through `disk_initialize`,
  which clears the cache before the SD driver's init (2026-10-08 review:
  it didn't, so FatFs read the old card's boot sector, FAT and folders
  from the cache, and a write would have put them on the new card).
  `CTRL_TRIM` invalidates FatFs's range (its first and last sector)
  before the SD driver's ioctl. The cache is one PSRAM block, allocated at
  the first mount (135,168 B), and the wrapper counts every read that
  reaches the SD driver (its sectors and its time), the cache on or off,
  and the mounts. **The device batch's switches:** `MSTREAM_SECTOR_CACHE=0`
  builds the stock driver alone; at runtime `gc0` turns it off (every call
  to the card, and a write drops the sectors it wrote from the cache),
  `gc1` on (it starts empty), `gc2` on with every hit read from the card
  again and compared (L1's soak: a difference is counted, the card's bytes
  returned, the sector dropped). A switch is taken at the next disk call,
  under FatFs's lock; a call reads it before it takes the clear asked with
  it (review: a call already under way could see the cache on before the
  clear and serve one sector written while it was off). Each prints the
  counts up to the switch, then they start again (review: they were
  printed after the reset was asked, not done, under "the counts start
  now"). The default is the spec's: on.
- **The card worker** (`app/CardWorker`): Thumbs' worker, generalised: a
  task on core 1, made for the first step and gone 3 s after the last, a
  6 KB internal stack, handed one step at a time by the loop (a function
  and its context) at the priority ScanScheduler says, raised or lowered
  while it runs (a cover drops to 0 when a list starts moving). A cover is
  one of its steps: Thumbs keeps its job and its PSRAM buffers, and no
  longer has a task of its own. `gs` prints its stack's least left over
  every life of the task (each life's own high-water mark starts again)
  and internal RAM's lowest while a step ran (sampled as each step starts
  and ends and at each loop pass during it), since the boot or `gs0`
  (2026-10-08 review: the last life's mark and the lowest since the boot,
  neither what L3 and L4 measure).
- **The loop's side** (`app/CardTasks`, every pass after the UI's): the
  finished step taken in (`stepDone()` with its time), the boot's walk 2
  s after the UI's first frame (`armWalk()`), the scan only once that walk
  ended (D's to-do is known then: 3.3.9), the work flags read only
  while the worker is free (a step may change them), the scheduler's
  `update()` with N7's inputs, then the step: `CardJobs::prepare()` on the
  loop (its memory, the scan's file), `step()` on the worker,
  `finish()` on the loop after it. Between steps, a chunk waiting 5 s with
  the worker free is written from the loop (the floor, a pause); one the
  card refused is offered again 30 s later (`CardJobs::idleFlush()`,
  `Config::retryMs`), not every pass (2026-10-08 review: every pass, each
  a FatFs open, about 1 s of the SD driver's retries on a pulled card and
  a write attempt on a full one); and the scan's memory goes back after
  10 s without a scan step.
- **The scheduler's inputs** (3.3.9's notes): `listMoving` the page's
  `animating()`; `input` what `stepIdle()` takes (a touch, the console, a
  key); the ring's `bufferedMsNow()` and a new `ringCapacityMs()` (1,486
  ms: 65,536 frames); `underrunsNow()`; `decodePassUs` a new peak the
  decode task raises after each pass and the loop takes back to 0
  (`takePassPeakUs()`); `decoderAtEnd` the heard track's source ended
  (`GaplessJoin::frozenLength()`), while playing only (a track paused or
  stopped at its end would otherwise hold the scan for good); `trackSeq`
  the backend's; `seeking` a start not taken up yet (`positionKnown()`
  false: a seek's, a skip's) and `seekSeq` a new `PlaybackController::
  seeks()` (the seeks that did something); `btSetup` a pairing or the
  link Paging, `btSeq` the link's ups and downs; `usb` external power;
  `battery` the reading as it came, -1 until the first (the UI's snapshot
  keeps its clamped copy).
- **The walk** (`CardJobs`): its first step writes the scan's chunk (N7's
  rule, and `append()` refuses during a walk), opens T (streamed on the
  first walk after a commit, through HIDX after), D (`KnownD`) and
  `walk.jnl` (`WalkSink`), then lists; each step after is one CardWalk
  step. **T that fails as it streams** (CardWalk's `Error::Transfer`)
  makes the walk start over without T, against no identity (D then
  records a walk against none: the next boot's walk is a first one and
  tries T again); T that won't even open is left out the same way. A walk
  asked while one runs runs again after it; a walk asked while D still
  has one to merge waits for that compaction, and a merge that fails
  isn't handed again until something asks for a compaction (the update
  step, `gr`, the next boot): on a full or pulled card the boot's walk and
  the scan then wait for the next boot (2026-10-08 review: it was handed
  every pass, each step a `LibraryWrite` that held the idle power-off
  off). A walk under way goes on to its end when the update step is
  asked: the compaction before the build can't run while it writes
  `walk.jnl`.
- **The walk's news is the files new to the index**, not to D: an index
  built from T, or walked from /music, lists files D has no row for yet,
  so the walk's sink counts its Added rows the index can't find
  (`Config::indexed`, `findTrack()` read on the worker: nothing changes
  the index while a step runs). That count is the toast ("Found 12 new
  tracks") and U11's (`buildAfterWalk()`); with none, the update step
  waits for the scan's end. The first boot on a card with no records thus
  walks /music into a path-named index (every track Pending), the
  background walk finds every file new to D but none new to the index,
  the scan reads them all (the playing track's, the queue's and the
  Library tab's first), and one update step shows their tags: no pause in
  between.
- **The scan.** Its "rest" is D's merged view (a `TagStore::View`) kept
  open across steps: each step looks at most 512 rows (the host tests
  use 3) for the next Pending one. It is closed before a walk or a
  compaction (they write what it reads) and opened again after, the chunk
  written first so the new View has what the scan read. A file a loop
  source named (the playing track, the queue's next 3 and 200, the
  Library tab's album, artist or folder, looked at in its first 512 rows)
  is taken at `f_stat`'s size and time; a rest row at the walk's (D's),
  and skipped when the file's size isn't that any more (changed since:
  the next walk sees it). An open View hasn't seen files the loop's
  sources read meanwhile: a read set of their path hashes (4,096 slots,
  half used at most; past that a file may be read twice, never missed)
  keeps the View from reading them again, cleared when a View opens. A
  file read is recorded Scanned (Ok or Partial) or Unreadable; a read
  that failed isn't recorded (read again at the next boot); a gone,
  changed or not-audio file is skipped. The chunk goes to `tags.jnl` at
  100 files, at half its 32 KB of entries, 5 s after its first record,
  at the View's end, and before any other job; a journal full to its 32
  chunks refuses it, kept, until the compaction the store then asks for.
- **What the loop learns from a scan step:** the index's track for the
  file stops being Pending (`LibraryIndex::clearPending()`, a new
  portable call that changes nothing else), so the playing track, the
  queue and the Library tab's page don't name it again; the playing
  track's record goes to `Library::setOverlay()` (N9's overlay, owned by
  Library in PSRAM, through `LibraryBuilder::viewOf()`), and Now Playing
  shows its tags at once.
- **The scan's end** (nothing for any source, no walk asked or running):
  the update step, when records reached `tags.jnl` since the last update
  step (`CardJobs::newRecords()`, a chunk the journal may still take
  included), the walk changed D, a Rescan was asked, or the boot loaded an
  index the scan had gone past (`Library::softStale()`: D's CRC or the
  journal's sequence differ from the saved soft inputs). A read whose
  chunk the card refused (full) isn't news: the build would find the track
  Pending again and the scan read it again (2026-10-08 review: an update
  step every few seconds, each holding the loop, on a full card). Not when
  the rest stopped on a read error (`Done::restFailed`: the card pulled;
  the next boot's scan's end has it). After an update step, built, failed
  or deferred, what the journal had asks for no other: only new records
  (or a walk's changes, a Rescan) do.
- **The update step at N10** (3.4.2; N12 moved its build and its save to
  the card worker behind a fence: 3.9): asked (the scan's end, U11, `gb`,
  g0's walk); the scan and the walk hold (`updateWanted`) while the
  journals are compacted (a worker step, the chunk with it); then, with
  the worker free and the safe point reached (not playing, or 20 s left at
  least and no seek in the last 2 s), the status line says "Updating
  library…" (0.6 s, so it is drawn before the loop stops), and
  `runUpdateStep()` checks the memory
  (`Library::roomToBuild()`: free PSRAM with the index's and the queue's
  bytes at least 1.1 x the index's size plus an eighth plus 96 KB, and
  the new track table, the builder's reservation from the headers' counts
  (`LibraryBuilder::trackSlots()`), fitting the old table's block, which
  the index keeps across the rebuild (`LibraryIndex::keepTrackBlock()`:
  the build or the load takes it again when the table fits, else frees it
  first), or the largest free block with a sixteenth to spare (ESP-IDF's
  TLSF rounds a request up to its next size class, a thirty-second,
  before it searches, so a free block of the table's own size isn't found
  for it); ESTIMATED, L4 measures), else writes the build-at-boot marker
  and toasts "Library updates at next boot". (2026-10-08 review: the
  check was "the largest block or the old track table", whose second
  half could never pass, the old table being exactly the new one's size
  before the margin; at 20k it deferred every update whose largest free
  block was under about 680 KB.) `gb!` asks the step with the check made
  to fail (L4.4). Then `rebuildLibrary()`: N3's remap through
  `queue.txt` around `Library::rebuild()`, **on the loop**, which is held
  for the build (about 9-14 s at 20k, ESTIMATED; under 0.1 s at 77
  tracks), as `g0` held it before; "Library updated" after. A card that
  doesn't answer (`/music`, or the records the boot opened: pulled while
  on) fails `rebuild()` before it touches the index, and the queue reads
  back the same ids (2026-10-08 review: the step walked the absent
  `/music` into an empty library and stopped the player). The build on
  the worker behind the "Updating library" fence, the queue's remap split
  around it, the save on the worker and Thumbs' pools freed are N12's.
- **`LibraryWrite`** holds while a compaction runs and while the update
  step is under way; the idle power-off's shutdown and the CPU speed's
  restart write the scan's chunk next to the queue (`flushNow()`: the
  worker's step finished first).
- **The status line** (3.3.6): on a Library page (the card's library, not
  a synthetic one) while there is something to say, a Small line across
  the list's width at y 220-239 (`uitext::kStatusW`), the list's band
  shortened by 20 px under it; drawn when its text changes, at most twice
  a second, never while the list moves (the band isn't changed under a
  fling either), again after a sheet or the dark. "Checking the card…"
  from the walk's arming to its end, "Reading tags N / M" while the scan
  has work (M: the files new to the index, the changed ones and the
  index's Pending ones, a walked index's being all of them, raised to N if
  passed), "Updating library…", "The last transfer
  didn't finish" when a plan is on the card and nothing else is said.
  The toasts are `Ui::note()`s. The Output tab's Library row and its
  Rescan tags button (3.3.6) aren't built (the console's `gr` is).
- **Covers** (2.14.3): an album with the transfer's thumbnail
  (`kTransferThumb`) reads `/.mstream/thumbs/<h>/<8 HEX>.565` first,
  keyed by its album folder's path hash (`thumbfile::pathHash()` of
  `/music/Artist/Album`, 2.3.3's), its header checked (MPTH v1, the hash;
  a "no picture" file is the device's marker, never the software's: a
  miss), never written; a miss falls to the folder's image as before.
  `hasCover()` is true for such an album with no image at all. The folder
  image is **streamed**: its header walked at offsets
  (`jpeg::parseFile()`, a new portable call: a few reads of at most 64
  bytes, segments skipped by their lengths, so EXIF past 64 KB costs
  nothing), TJpgDec fed from a 16 KB PSRAM buffer refilled from the file;
  the whole file is never in PSRAM and the 2 MB cap is gone. A read that
  fails, in the header's walk (`jpeg::Info::readFailed`) or mid-decode,
  is the card's, not the picture's: not remembered as undecodable
  (2026-10-08 review: one in the header's walk read as "not a JPEG", and
  its card copy said "no picture" for good).
- **The console** (3.3.6): `gr`, `gr!`, `gw`, `gb` and `gv` are the
  worker's jobs now; g0 on the card asks a walk and the update step after
  it (the flash builds at once, as before); `gs` adds the scheduler's
  waits (seconds per reason), each job's steps (count, mean, longest), the
  worker's stack's least left and the internal RAM's lowest while a step
  ran, the jobs' counts and the cache's; `gs0` starts the waits, the steps
  and the worker's figures again (L3's figures one condition at a time);
  `gb!` asks the update step with its memory check made to fail (L4.4's
  deferral); `gs`, `gt` and `gl` wait for the worker's step first (they
  read the card's records themselves). New: `gc` (the cache: `gc0`,
  `gc1`, `gc2`) and `gl` (L0's bench; `glw` with the walks), 6.3.
- **Sizes (MEASURED, the build).** IRAM unchanged in both builds
  (`.iram0.vectors` 1,028 + `.iram0.text` 124,867 = 125,895 B); internal
  DRAM +472 B (`.dram0.data` 24,328 to 24,440, `.dram0.bss` 32,168 to
  32,528: the worker's handle, the wrapper's state, the sector cache's
  object, a few counters); flash `.text` +76,244 B and `.rodata` +16,668 B
  (1,667,560 to 1,743,804 in both builds; 613,928 to 630,596 in core2,
  613,960 to 630,628 in core2-dio), 93,024 B in all, the app 2.39 to 2.48
  MB, 41% of its slot; `iram_diet`, `cache_guard`, `flash_guard` and
  `version` pass in both. After the 2026-10-08 review's fixes: IRAM
  unchanged, internal DRAM -32 B (`.dram0.data` 24,488, `.dram0.bss`
  32,448), flash +3,308 B in core2 (`.text` 1,746,136, `.rodata` 631,572)
  and +3,332 B in core2-dio (1,746,128 and 631,636), the app 2.49 MB, every
  guard passing. PSRAM
  (from the code; L0 and L3 measure): the cache 135,168 B on the card; the
  walk about 98 KB while it runs; the scan about 85 KB and its View's
  merge memory; the lister's `FIL`, `FF_DIR` and `FILINFO` about 4.5 KB;
  Library's root (about 4 KB), overlay and run (about 7 KB); CardTasks
  about 2 KB.
- **Not as 3.2-3.4 said:** the update step's build and save ran on the
  loop (above: N12 moved them to the card worker, and set ScanScheduler's
  `build`, `save` and `updating` inputs, 3.9); on the flash fallback
  nothing of the records exists (no D, no walk, no scan: today's signature
  walk), not the POSIX walk 3.2.3 kept for it; `queue.txt` keeps its own
  write-aside rule, not 2.12.6's twins; the DJNB check is never asked (no
  AutoDJ engine yet). (N12's review found the compaction's frame 13 KB
  deep on the 6 KB card worker: fixed, 3.9.)
- **Proven on the host:** `test_card_jobs` (9 tests): every row of
  3.2.2's table and `matches()`'s inputs; the root's election, T against
  its COMP entry, LIBR, the plan; a hand-filled card (tagged corpus files
  among noise) walked, scanned once each, compacted and built into an
  index of their tags, then an unchanged boot that opens no file, then a
  hand-copied album walked and read alone; the walk's files new to the
  index; the loop's sources (a file named at its f_stat size and time,
  the rest not reading it again, a gone file skipped); the chunk's
  appends, a full journal's refusal and the compaction after it with
  nothing lost; a file changed or gone after the walk, then a Rescan; a
  transfer card (no scan, the build from T, Verify finding the one bad
  qfp, Rescan everything); a T whose STRS fails its CRC walked again
  without it; and since the 2026-10-08 review a card that refuses writes
  (a walk's failed merge handed once, the refused chunk offered again
  only after `retryMs`, a refused read no news for the update step) and a
  rest that ends on a pulled card (`restFailed`), the soft inputs a build
  that left the journals out saves, T's record count. `test_sector_cache`
  (the wrapper's rules, `CachedDrive`: a mount clears, a write with the
  cache off drops, on again starts empty, verify, the deferred reset),
  `test_fat_model` (a card swapped under the wrapper: FatFs's own remount
  reads the new card, and a write there leaves the image FatFs without
  the cache leaves; without the clear it fails), `test_library_index`
  (`peek()`, `clearPending()`, the kept track block), `test_library_builder`
  (`trackSlots()`), `test_thumbs` (`parseFile()` against `parse()`, its
  failed reads), `test_ui_library` (`gc`, `gl`, `gs0`, `gb!`),
  `test_playback` (`seeks()`). Not on the host: FatFs on the card, the SD
  driver, the task, the UI; 6.3.1 lists what the device batch checks.

### 3.9 As built (N12): LibraryUpdate, the build on the card worker

The boot decision and the update step (3.2.2, 3.4.2) are one portable
state machine, `lib/core/LibraryUpdate`, host-tested like `IdlePolicy`
(`test_library_update`), and its glue: the build and the save are card
worker steps, the loop goes on behind a fence. Built for `core2` and
`core2-dio` with every guard, **not flashed**. What the code decided where
3.2-3.4 left room:

- **The files.** lib/core: `LibraryUpdate` (the boot: `library.tmp`
  settled with a whole-file check, the marker, `library.idx`'s header,
  `libraryboot::decide()`'s table, then the load, the build from the
  records or the walk, the save, the marker's removal; the update step's
  phases, the safe point, the memory check, the deferral, the build and
  the save as steps), `QueueRemap`'s `Carry` (N3's remap in two halves),
  `PlaybackController::setFenced()`, `TrackCatalog::Held`,
  `ThumbCache::release()`, `EntryStart::rekey()`, `librarytext::Sources`
  and the Output tab's texts. src: `app/Library` (the card's boot and the
  fence's readers; N10's `buildCard()`, `saveCard()`, `roomToBuild()` and
  `deferToBoot()` are `LibraryUpdate`'s now), `app/CardTasks` (the phases
  each pass, the Build and Save steps), `app/QueueStore`
  (`remapBegin()`/`remapFinish()`), `main.cpp` (the fence: `enterFence()`,
  `leaveFence()`, what the loop shows meanwhile), `ui/Ui`, `ui/Thumbs`,
  the Library and Queue pages, `ui/OutputPage` (the Library row).
  `libraryboot` (3.2.2's table) stays where N10 put it: the boot calls it.
- **The boot** (`LibraryUpdate::boot()`, before the UI, as N10's but
  portable): one call does what `Library::beginCard()` did after opening
  the card, and returns what it did for the log lines (6.3.1's are
  unchanged, and one more: `library.idx ... (library.tmp taken: a cut fell
  mid-save)`). The walk of a card with no records is the caller's
  (`Config::walk`: the firmware's VFS walk; `test_library_update` walks
  FakeFat's tree). The marker's build out of PSRAM on the boot's fresh
  heap (N10 left no library then, on every boot: the marker stayed, and
  no update step could build it either), `library.idx` matching the card:
  it is loaded as the Load row would (`Booted::loadedShort`), the marker
  removed, and this session's short memory checks end their steps without
  writing it again (`Step::markerSkipped`, no toast; `gb!` still writes
  it). A build that met a read error (`Booted::readErrors`: the card?) is
  saved with the journal's sequence `kJournalsLeftOut`, as one that left
  records out: the next boot loads it soft-stale and rebuilds.
- **The update step, pass by pass** (`update()` with the worker's, the
  jobs', the player's and the memory's state; `Out` says what to hold and
  what the loop does now, each once):
  1. **Asked** (`ask()`: the scan's end, U11, `gb`, `gb!`, g0's walk): the
     scan and new walks hold; a walk under way goes on to its end; the
     journals are compacted (the worker's Compact step; one that fails
     leaves the build to `tags.bin` alone, as N10's); with the worker
     free, the safe point holds (a CPU speed's restart asked counts as
     none: its pause would make one, and the restart would cut the build),
     the worker's task is there (`Out::wantWorker`: `CardWorker::ensure()`
     makes it, since it ends 3 s after its last step, and keeps it to the
     build's hand-off; no internal RAM for its 6 KB stack and the step
     waits here, the index as it is, never behind a fence whose build
     can't start), the card answers (`/music` exists, T and D open: a
     pulled card fails the step here, `Do::Failed`, the index untouched),
     then the memory check (N10's, with Thumbs' pools counted too: the
     index, the queue's entries and the pools, about 315 KB, go before the
     build). Short, or `gb!`: the marker, `Do::Deferred` and the toast
     (not after a boot whose own marker build ran short: above).
  2. **The fence** (`Do::Fence`, one pass, `main.cpp`'s `enterFence()`):
     the queue as it is kept for the loop (`Frozen`: its position, size,
     keys and versions, the sleep timer's last-of-queue and last-of-album);
     `QueueStore::remapBegin()` (`queue.txt` flushed, the queue's memory
     given back, the player fenced; a card that refuses `queue.txt`: the
     queue's text in PSRAM instead, held through the build, so only up to
     what the memory check had to spare, `LibraryUpdate::spare()`);
     `Library::fence()` (the playing track's names copied, the catalog
     without its index); Thumbs' pools lent; `CardTasks::fenceUp()`: the
     scan's View closed, its chunk out, its memory back, and the old index
     cleared (`fencedUp()`; its track table's block kept for the build,
     N10's `keepTrackBlock()`; `index()` nullptr from here); then the UI's
     lists to their line (after the clear: they count no rows, so the
     "Updating the library…" line shows, not the old index's rows drawn
     blank). The queue can't be carried (no PSRAM for the carry, or its
     text over the spare): nothing is given back (`Carry::begin()` false,
     the queue as it was), `cantFence()`, deferred to the boot as a short
     PSRAM.
  3. **The build** (`Out::build`: ScanScheduler hands `Job::Build` at
     priority 1, before anything, a moving list included): on the worker,
     T and D opened there (their `FIL`s in PSRAM; the opens' 512 B
     long-name buffer on its stack), `LibraryBuilder` as N10 ran it, a T
     that fails at its end restarts from D alone (3.4.1) and is left out
     for the session, a card with no records walks `/music` through the
     same VFS walk as the boot's, the inputs to save taken at its end. A
     read that fails (the four files watched: the walkers say `Why::Io`,
     but D's rows and its folders' facts read on without a word, and a
     builder that restarts without both says "no records") is the card's,
     pulled or failing, not a file's checks: on D's files, or on T with no
     whole build from D after it, the step fails (`Step::cardGone`,
     `readErrors`: nothing walked, which on a pulled card gave an empty
     library and "Library updated", nor saved, which after a glitch the
     card came back from put a path-named index over `library.idx`; T not
     marked bad; the next boot loads the last `library.idx`); on T with D
     read whole, the build from D alone stands, T is tried again next
     time, and the save marks it as records left out (the next boot loads
     it soft-stale and its scan's end reads T again).
  4. **Live** (`Do::Live`, one pass, `leaveFence()`): the catalog's index
     back, `CardTasks::live()` (`lived()`: the index readable; the jobs
     start over with it; "Library updated"), `remapFinish()` (`queue.txt`
     read back with the new ids, the player unfenced and told),
     `sleepEntry` re-keyed when the same file plays on, the lengths
     learned forgotten, Thumbs' pools back, `libraryChanged()`.
  5. **The save** (`Out::save`: `Job::Save` at priority 0, under the
     background work's yields but not the battery floor; covers may run
     before it): `library.tmp` written, 2.12.6's `replace()`, then the
     marker removed if one is on the card (a deferral's earlier in the
     session included); `Do::Saved`.
  From the fence to the save's end `Out::updating` holds the walk, the
  compactions, the scan and the DJNB check (ScanScheduler's `updating`
  input: a compaction asked meanwhile, `gr`'s or the journal's, runs
  after the save), and `Out::libraryWrite` holds IdlePolicy's
  `LibraryWrite`. ScanScheduler's `build`, `save` and `updating` inputs,
  which N10 never set, are these. A boot that loaded a soft-stale index
  asks for the step at the first scan's end after its walk, even with
  nothing to scan (`CardTasks::begin()`: N10's `scanOver_` started true,
  so a card with no scan work, a transfer card after a cut mid-build say,
  never rebuilt it).
- **The fence's span** is `fencedUp()` to `lived()`: the pass that puts it
  up still reads the old index (the queue's flush names its paths, the
  sleep timer's last-of-album reads it), the pass that takes it down reads
  the new one after `lived()`. Between them `Library::index()` is nullptr
  and the catalog has no index: every reader on the loop sees "no library"
  by construction, as at a boot with none (the pages, Thumbs, the scan's
  sources, the console), and `test_library_update` checks it at every
  read the build makes of the card and every block it takes.
- **What the loop shows behind the fence** (`AppState::libraryFenced`):
  Now Playing keeps its track (the snapshot's queue fields are `Frozen`'s,
  the time the backend's) and its names (`TrackCatalog::Held`, taken
  before the fence: title, artist, album, its artist line, year, length;
  no path); the Library and Queue lists show "Updating the library…" (the
  Library tab back to its root, its sheets and jump grid closed; the
  status line says "Updating library…" from the moment the step waits
  only for the safe point or the worker to Live); covers are the
  placeholder. The tab bar and the sleep timer read `Frozen`; the buttons'
  "nothing to play" too (B pauses).
- **The player behind the fence** (`PlaybackController::setFenced()`):
  the queue is empty and the catalog has no paths, so it reads neither:
  no heard join is taken (the backend keeps it), no word on what follows
  goes (the backend keeps the one it had), `update()` does nothing. The
  actions find no queue and do nothing, but pause and resume of the held
  track: the listener can always stop the sound. After the fence,
  `queueReplaced()` takes a join heard meanwhile by its entry's path (the
  `Offered` word keeps its path's FNV-1a, since every key and id changed;
  the same file next also keeps its token, so a decode-ahead isn't cut),
  and a track that ended inside the fence with nothing joined (the build
  outlasted the safe point's margin with gapless off, the timer's end, the
  queue's end; or a paused track resumed near its end) **starts
  nothing**: the entry after it is cued at 0:00, paused (stopped at the
  queue's end with repeat Off), and a play starts it (`fenceStops()`,
  logged). With gapless on the next track joins as it would, and plays on.
- **What waits** (`waitsForLibrary()`: a note, "Updating the library: a
  moment"): the UI's next, prev, shuffle and Shuffle all (the seek bar
  shows no knob: the catalog has no path to ask), the Queue tab's edits
  (no Edit offered, its size being `Frozen`'s; a selection open at the
  fence ends; the bar's Remove, Play next and Clear refused with the
  note; `PlaybackController` refuses the queue's edits behind the fence
  too: its Clear queue stopped the music, cleared nothing, and the queue
  came back whole after), the console's `n`, `p`, `l`, `i`, `b`, `q`,
  `R`, `G`, `g` and `j` (`g` and `j` until the save's end: `gs` would
  wait for the worker's build); the CPU speed's restart while
  `LibraryWrite` holds (`powerchoice::cpuRestartDue()`: it was asked
  before the fence, the pause made the safe point, and it cut the build
  3 s later; now it waits, and an update step asked meanwhile waits for
  it).
- **The safe point** is N10's, and a play waiting for the headphones
  isn't one (its start would need a path). The last seek's time is the
  machine's (`In::seekSeq`).
- **The Output tab's Library row** (3.3.6; it fits the budgets: Sizes
  below), before About: "Library: 19,410 tracks" (Body; "19,410 tracks"
  when that doesn't fit), where their names come from
  (`librarytext::sourcesText()`: "18,000 from the transfer, 1,400 read
  here, 10 without tags", then "18,000 transfer, 1,400 here, 10 none",
  then "99% tagged", the longest that fits; "names from the files" when
  no track has a record), counted once a build or a load (the index's
  build stamp); "tags need a card" on the flash, "updating…" behind the
  fence. Its "Rescan" pill (68 px: its one word) opens a dialog, "Rescan
  tags?", whose Rescan is `gr`'s (`CardTasks::rescan(false)`: the
  device's own records only) and a toast, "Reading the tags again"; the
  status line then counts the scan.
- **Not as 3.2-3.4 said:** AutoDJ's maps, its join and the DJNB check
  aren't there (no AutoDJ engine yet); the queue is carried by N3's
  `Carry` (its read pre-sized from the file's header, as 3.4.2 asks), not
  `restore()`'s code; with no records at the step (an update asked before
  the boot's walk made D) the walk runs on the worker, not the loop.
- **Sizes (MEASURED, the build; against b2c633c built the same way).**
  IRAM unchanged in both builds (`.iram0.vectors` 1,028 + `.iram0.text`
  124,867 = 125,895 B); internal DRAM +112 B (`.dram0.data` 24,488 to
  24,552, `.dram0.bss` 32,448 to 32,496: the fence's `Frozen` copy, the
  Library row's counts, the player's fence and its words' path hashes, a
  few pointers); flash `.text` +8,320 B (1,746,128 to 1,754,448 in both
  builds) and `.rodata` +4,236 B in core2 (631,588 to 635,824) and
  +4,268 B in core2-dio (631,620 to 635,888), the app 2.49 to 2.50 MB
  (`firmware.bin` 2,606,016 to 2,619,520 B in core2), 42% of its slot;
  `iram_diet`, `cache_guard`, `flash_guard` and `version` pass in both.
  The review's fixes, against 3ae7cb6 the same way: IRAM and internal
  DRAM unchanged, flash `.text` +1,484 B (1,755,932 in both builds) and
  `.rodata` +1,024 B in both (636,848 in core2), `firmware.bin` 2,622,240 B
  in core2; every guard passes.
  PSRAM (from the code): `LibraryUpdate` and its step about 0.4 KB, the
  held names about 1 KB, the carry about 0.1 KB, and when the card can't
  take `queue.txt` the queue's text through the build (up to about 400 KB
  for 5,000 long paths, only within `spare()`: the memory check counts
  the queue's entries as freed, not this); lent to the build and given
  back: the queue's entries (up to 120 KB) and Thumbs' pools (315,952 B).
- **The card worker's stack (ESTIMATED from `-fstack-usage`; L4
  measures).** The build's deepest path is its views' sort at the end
  (`LibraryIndex::finish()`, `buildViews()` 640 B, `std::sort`'s
  introsort 112 B a level, at most about 30 levels at 20k before it turns
  to heapsort): about 2.7 KB typical, 4.5 KB at worst of the 6 KB with the
  frames above it (`stepBuild()` 128, `buildIndex()` 208 and since the
  review about 80 B more for its four files' read watchers, the
  builder's 208, the task's); the merge's reads (a walker's 352 B, FatFs
  and the SD driver below it) about 2 KB; the save about 1.5 KB (`save()` 368 B, a
  rename's 640 B and FatFs's 512 B long-name buffer). Tight but under;
  `gs`'s least left after a build is L4's figure.
- **The compaction's stack (found by this review).** The same build put
  `TagStore::compact()` at a 13,360 B frame: `new (p) CompactWork()`'s
  value-initialization made xtensa's GCC build the 13 KB object on the
  stack and copy it into its PSRAM block. The compaction is a card worker
  step since N10 (6 KB) and the boot's ran on the loop task (8 KB): the
  first compaction on the device would have overflowed either. Now the
  block is zeroed and the object default-initialized in place (the same
  meaning): 944 B; `DeviceCheck` (`tags.tmp`'s check at the boot) the same
  way, 4,672 B to 64 B. The largest frames left in our code that the
  firmware links are the console's (`gt`'s `mptg::check()` 3.8 KB, `gt`'s
  dump 1.3 KB: the loop's) and TagScan's 1.3 KB (the worker's scan, 3.3.4's
  budget).
- **Proven on the host:** `test_library_update` (13 tests, like
  `test_idle_policy`: the scheduler, CardJobs on FakeFat's trees and the
  records and `library.idx` on CutFs, a worker whose steps take passes,
  the loop's catalog): the safe point and the memory check at their
  edges; every row of 3.2.2 with real files (no records: walked, all
  Pending; matching: loaded, soft-stale when the journal moved; the
  marker: built, removed; another identity, `/.mstream` gone: from T and
  D, from D; v5, unreadable, missing: built, or walked with no records;
  NoMemory); `library.tmp` whole and taken, torn and removed; a deferral
  (short PSRAM, `gb!`, no PSRAM to carry the queue) then the boot that
  builds, and a deferral then memory in the same session; a track near its
  end (19,999 ms left, the
  next track's unknown length, a seek's 2 s) and a play waiting for the
  headphones; a compaction, a Rescan and a walk asked during a build,
  each after the save; a T whose strings fail at its end (the build from
  D alone, T out for the session, the next boot's load and the marker's
  build alike); a power cut at every step of an update, with and without
  a marker, each way a card comes back (InOrder, LoseUnsynced, Torn): a
  whole library every time, old ones soft-stale and rebuilt, `library.tmp`
  taken when the cut fell after its sync, no chain freed under another
  entry; the fence at every read and allocation of the build; the
  phases' order, LibraryWrite's span, and a pulled card failing before
  the fence; the review's three more: a card that fails while the build
  reads it (pulled, or one read of D: nothing walked or saved, T not
  marked bad, the next boot loads the last `library.idx`; one read of T:
  built from D alone, saved soft-stale, the next boot's update reads T
  again), the worker's task before the fence (no internal RAM: the step
  waits, the index untouched), and the marker's build out of PSRAM at the
  boot (loaded instead, the marker gone, not written again that
  session; `gb!` writes it); and `spareOf()` at its edges.
  `test_gapless_player` (4 more): every id renumbered behind
  the fence with the join after it kept (one play, no cut); a join heard
  inside the fence taken after it by its path; a track that ends inside
  the fence with nothing after it starting nothing (cued paused, or
  stopped at the queue's end); only pause and resume acting inside it,
  the queue's edits refused. `test_queue` (the remap in two halves,
  through the file and through memory, and a queue it can't carry: over
  the spare, or no memory, nothing given back), `test_power_choices`
  (the CPU restart waits for `LibraryWrite`), `test_thumbs` (the pools
  lent and back), `test_ui_library`
  (the held copy; the Library row's and the fence's texts),
  `test_sleep_timer` (`rekey()`). Not on the host: the task, FatFs on the
  card, the UI, `CardTasks` (the soft-stale index at the scan's end, the
  worker kept to the hand-off); 6.3.1's L4 lists what the device batch
  checks.

---

## 4. mStream's API: what the transfer uses, and the additions

The user's note of 2026-10-01 put the first version in mstream-terminal
with no new mStream APIs. **That holds:** the routes below are enough for
the whole contract when the software reads the tags from the card bytes
(2.7). The additions in 4.2 are upgrades, each with a fallback.

### 4.1 What it uses today (mStream 6.27.0 or later; READ at 926b97b1)

| Need | Route | What to know |
|---|---|---|
| Sign in | `POST /api/v1/auth/login` → `{token, vpaths}` | API calls carry `x-access-token`; `/media` and `/transcode` take `?token=`. The terminal's client does all of this already. |
| Capabilities | `GET /api/v1/ping` | `sync: true` (the manifest route exists); `discovery` (embeddings switched on); `discoveryReady` (some exist); `transcode`; `supportedAudioFiles`. No stable server id (A6). |
| The file list, identity, lite metadata | `POST /api/v1/sync/manifest` `{cursor?, limit? 1-5000 (2000), ignoreVPaths?}` → `{revision, scanning, next, entries[]}` | The ETag is `revision`; `If-None-Match` on the first page gives a 304. `revision` covers the row count, max id, max mtime and the tracks with art. Entries: `filepath`, `id`, `file-size`, `modified` (ms), `hash`, `audio-hash`, `hash-v`, `format`, `album-id`, `artist-id`, `created-at`, and the lite `metadata`. `hash` is an MD5 of every byte under the server's 25 MB threshold, a sampled digest above it. `scanning: true`: a missing path is not a deletion. |
| The rest of the metadata | `POST /api/v1/db/metadata/batch` (paths in, chunked) | Adds `sample-rate`, `channels`, `bit-depth` (the conversion decision), `bpm-source`, `track-total`, `disc-total`, `composer`, the MusicBrainz and ISRC ids, the real `artists` credits. |
| The bytes | `GET /media/<vpath>/<rel>?token=` | `express.static`: byte-exact, `Range` works, so downloads resume. |
| A conversion | `GET /transcode/<vpath>/<rel>?codec=opus&bitrate=192k` | ffmpeg to a pipe: codecs `mp3`, `opus`, `aac`; bitrates 64k, 96k, 128k, 192k; no sample-rate option; pictures dropped (`-vn`); text tags through ffmpeg's default mapping; streamed with no length and no resume. **The player plays mStream's Opus output sample-exact and gapless** (OPUS.md), so Opus 192k is the conversion that needs nothing new. An MP3 to a pipe probably gets no Xing/LAME header (ESTIMATED, not tested), which makes its length a CBR estimate. |
| Covers | `GET /album-art/<file>`; `HEAD /media/<vpath>/<dir>/<name>` | The original image (`?compress=l` and `?compress=s` serve 256 px and 92 px JPEG copies when made). Folder images can't be listed; the software probes the names (2.14.2). |
| Embeddings | `POST /api/v1/discovery/local/embeddings` `{filePaths: 1..8}` | `{model:{id,version}, dim, tracks:[{filePath, audioHash, notAnalyzed, embedding}]}`, the embedding base64 of 1,280 little-endian float32, L2-normalised. 403 when discovery is off; **one unresolvable path fails the whole call (404)**, so only manifest paths are sent. For 10k tracks: about 1,250 calls and 68 MB of base64, 1-3 min on a LAN (ESTIMATED). |

Not usable for the table: `/discovery/local/similar/tracks` ranks against
the server's whole library (at most 100), not the card's selection.

### 4.2 The additions, best first

None is required for v1. Effort is ESTIMATED, in agent-days on mStream,
with tests.

| # | Addition | Why | Fallback without it | Size |
|---|---|---|---|---|
| A1 | Manifest entries gain `bpm-source`, `sample-rate`, `channels`, `bit-depth`, `track-total`, `disc-total` and `art-source` (`album_art_source`); all are on `t.*`, which the query already selects. A ping key `syncManifest: 2`, `sync: true` kept. | Removes the `metadata/batch` round trips; tells analysed BPM from tag BPM and folder art from embedded. | `metadata/batch` per page. | 0.25 |
| A2 | Bulk embeddings: the cap from 8 to 256, `audioHashes` accepted as well as paths, optionally `Accept: application/octet-stream` (dim × 4 bytes per row). | 1,250 calls become 40; 68 MB becomes 51 MB. | 8 per call, 4-8 calls in parallel. | 0.5 |
| A3 | A `rules` block on the manifest's first page: `artistSplitExceptions` (admin-only today, default empty), the split delimiters, the ignored articles, `albumArtPriority`, the scanner engine and its lofty version, the schema version. | Stage B's credits and any later rule change agree on both sides without a code change. | The defaults in part 5. | 0.25 |
| A4 | `includeImages: true` on `POST /api/v1/file-explorer`, returning `.jpg`, `.jpeg` and `.png` names with sizes. | The folder-first cover source exact, "the largest other .jpg" included. | HEAD probes of the named files. | 0.25 |
| A5 | `/transcode` bitrates 256k and 320k, and an optional `sampleRate`. | MP3 as the conversion target, if wanted over Opus (part 7, U3). | Opus 192k. | 0.25 |
| A6 | A stable server instance id in ping. mStream already makes one (`discovery.mdns.instanceId`, a random UUID in its config, sent only over mDNS). | Binds a card to a server (MSMF `serverInstance`) without storing a URL or a token on the card, whichever URL reaches it. | serverUrlKey, the hash of the base URL (2.5.1), which v1 always writes; a second URL for the same server asks once. | 0.25 |
| A7 | Raw per-file tags in the manifest (`tag_album`, `tag_album_artist`, `tag_compilation`, the release id, raw credits). | Only if a route ever needs the file's own tags without its bytes. A transfer downloads the bytes and reads them, so likely never. | Reading the bytes (2.7). | 0.5-1 |
| A8 | A server-side table, `POST /api/v1/sync/autodj` (autodj research section 6): the selection in, one MPDJ out, built in a worker and cached. | Only if building in the terminal is unwanted. | The terminal builds it (about 3-25 s of compute at 10k, ESTIMATED). | 2-3 |
| A9 | ReplayGain album gain and peaks stored (a migration and both scanner engines). | Only if the device will apply album gain (part 7, U16); the records carry it from the file either way. | The device's and the software's own reading. | 1-2 |
| A10 | A manifest `revision` that sees changes in place: add `SUM(t.modified)` to the four aggregates, or a hash over (id, modified, file_hash). | Today's revision misses a file replaced by one whose mtime isn't the library's newest, since the scanner's UPSERT keeps the row's id (2.10.2). | A full manifest read every 7 days or 10th run, and in the deep check. | 0.25 |

A1-A6 together: about 1.5-2 days; A10 a quarter day more.

### 4.3 Bugs and quirks seen (read-only; for mStream's backlog)

- `'replaygain-track': row.replaygain_track_db || null` turns 0.00 dB into
  null; `??` would keep it (`src/api/db.js`, `renderMetadataObj`).
- The manifest's `artists` is always `[artist]`, because `manifestPage`
  doesn't run the credits enrichment (`src/api/sync.js`).
- **The two scanner engines likely disagree on several genres** (not
  run): the Rust engine keeps the first Genre item (lofty splits TCON on
  NUL and "(n)" into separate items, and `Tag::genre()` returns the
  first), then splits it on `, ; /`; the JS engine joins every value. No
  parity fixture covers it (part 7, U10).
- An album-art backfill changes `revision` (it counts the tracks with
  art), so a periodic run re-plans; the software redoes only thumbnails
  then (2.10.2).
- The other way round, `revision` (count, max id, max `modified`, the
  tracks with art) misses a file changed in place whose new mtime isn't
  the library's newest: the scanner's UPSERT keeps the row's id
  (`src/db/scanner.mjs`). A client trusting the 304 keeps the old file
  (A10).
- `parse_replaygain_db` (rust-parser) strips only `dB` and `db`, so
  `-6.5 DB` reads as no gain there; the records' rule (5.3) takes any
  case.

---

## 5. The tag rules both sides follow

The reference is mStream's default scanner: the rust-parser with lofty
0.25 in relaxed mode, behind its ID3v2 repair pass (READ at 926b97b1;
the JS engine, music-metadata, runs only when no rust-parser binary
works). Both producers follow 5.1-5.3 when they write a record; the
builder follows 5.4 when it shows one. `readRules` = 1 (2.6.1) names this
version of the rules; a change that alters records bumps it, and the
device rescans its own records of an older version.

### 5.1 Which tag is read

**MP3.** lofty holds the tags in the order ID3v2, ID3v1, APE; mStream
takes the primary tag (ID3v2), else the first present. So without ID3v2,
**ID3v1 beats APE**; APE is read only when there is neither.

- **The ID3v1 fill:** unless the chosen tag *is* ID3v1, a present ID3v1
  fills a blank title, artist, album or genre (empty or whitespace only),
  and an absent year or track number. ID3v1 strings are trimmed of
  whitespace, whether they fill or are the chosen tag; its genre byte maps
  through the 192-entry table (255: none).
- **Several ID3v2 tags at the head** are merged frame by frame, the later
  tag's frame replacing an earlier one of the same id (lofty's `insert`;
  READ, not run: the parity corpus pins it). An ID3v2 is also looked for
  in junk before the first frame, up to lofty's `max_junk_bytes`.
- Fields ID3v1 can't hold (album artist, composer, sort names, ids,
  ReplayGain, BPM, key, compilation, totals) come from the chosen tag
  only.

**FLAC.** The Vorbis comments are the tag; a front ID3v2 is used only when
the file has no comment block and no PICTURE block.

**Opus.** OpusTags (Vorbis comments, across pages).

### 5.2 Text

- **Latin-1 is ISO-8859-1:** each byte becomes U+00xx, trailing NULs
  trimmed (lofty's `latin1_decode`). Bytes 0x80-0x9F become U+0080-U+009F
  in the record, *not* cp1252. A device MAY draw U+0080-U+009F as their
  cp1252 characters on screen; the record keeps them.
- **UTF-16** with a BOM, and UTF-16BE; **UTF-8**.
- **Repairs** (mStream's ID3v2 repair pass): invalid UTF-8 becomes U+FFFD
  per maximal invalid subpart (as Rust's `String::from_utf8_lossy`); an
  odd-length UTF-16 frame's stray byte and an unpaired surrogate become
  U+FFFD; a frame running past the tag's end is cut there; a v2.4 frame
  size that isn't syncsafe is re-read as a plain integer when that lands
  on a frame boundary; v2.4 tag-level unsynchronisation is undone frame by
  frame.
- **Several values:** an ID3v2 text frame splits on NUL in every version
  (an empty trailing value dropped); a Vorbis key repeats.
- Then 2.3.6 applies: control characters to spaces, empty values dropped,
  repeats dropped, the length limits.

### 5.3 What a record holds, field by field

| Field | Rule |
|---|---|
| title | The first Title value, as is (not trimmed, not folded). |
| artist | Every TrackArtist value (TPE1, ARTIST), as is. |
| album | The first Album value, as is. |
| albumArtist | ID3v2: the TPE2 values; if none, the first non-blank TXXX `ALBUMARTIST` or `ALBUM ARTIST` value. Vorbis and APE: the `ALBUMARTIST` and `ALBUM ARTIST` values. |
| genre | Every Genre item, each translated: ID3v2 TCON's `(n)` and `(n)Refinement` to the names of the 192-entry table, `RX` to Remix, `CR` to Cover; an ID3v1 genre byte through the table. The split on `, ; /` is the builder's (5.4). |
| composer | Every Composer value (TCOM, COMPOSER). |
| sort fields | The first value of TSOT/TITLESORT, TSOP/ARTISTSORT, TSOA/ALBUMSORT, TSO2/ALBUMARTISTSORT. |
| mbAlbumId, mbRecordingId | lofty's MusicBrainzReleaseId and MusicBrainzRecordingId items, trimmed; empty is none. |
| year | The Year item, else RecordingDate (ID3v2.3 TYER reads as TDRC; Vorbis YEAR is Year, DATE RecordingDate): after leading whitespace, the first four characters must be ASCII digits, and they are the year; otherwise none. Then the ID3v1 fill. |
| track, trackTotal, disc, discTotal | lofty's readers first ("N/M" in TRCK and TPOS; Vorbis TRACKNUMBER, TRACKTOTAL or TOTALTRACKS, DISCNUMBER, DISCTOTAL or TOTALDISCS); else the raw string split once on `/`, each part trimmed and parsed as an integer; anything else (`A1`) is none. 0 is none; values above 65,535 are 65,535. |
| durationMs | The producer's measure, within ±100 ms of the played length: the device's helpers trim the MP3 encoder delay and padding (GAPLESS.md section 4), lofty doesn't (frames × samples per frame ÷ rate); FLAC from STREAMINFO; Opus from the last granule minus the pre-skip. |
| bpm10 | Vorbis `BPM`, else ID3v2 `TBPM`: a decimal by the number rule below, rounded to an integer, kept only from 20 to 300 (mStream's rule: round first, then the range); bpm10 = that × 10. (The server's analysed BPM, 2.7: round(bpm × 10), BPM_ANALYSED.) |
| camelot | TKEY, or Vorbis `INITIALKEY` or `KEY`: trimmed, its first 12 characters, matched case-insensitively against the aliases of mStream's `CAMELOT_TO_KEYS` (`src/api/random.js`: `8A`, `A minor`, `Am`, `Amin`, …); no match is 0. |
| ReplayGain | `REPLAYGAIN_TRACK_GAIN`, `REPLAYGAIN_ALBUM_GAIN` (TXXX, Vorbis, APE): one trailing `dB` stripped (any ASCII case), then a decimal by the number rule below, in hundredths of a dB; outside the i16 range: absent. The peaks `REPLAYGAIN_TRACK_PEAK`, `REPLAYGAIN_ALBUM_PEAK`: the number rule in ten-thousandths, saturating at 65,535; negative: absent. Opus `R128_TRACK_GAIN` and `R128_ALBUM_GAIN`: an integer (`[+-]?[0-9]+`) in the i16 range, converted by 2.6.4's integer formula (RG_FROM_R128). mStream reads only the track gain; the record keeps all four. |
| compilation | TCMP or COMPILATION: `1` or `true` (any case) is 1; `0` or `false` is 2 ("said no"); anything else, or none, 0. mStream keeps only "yes". |
| picture | Every non-empty embedded picture is seen, except a compressed or encrypted ID3v2 frame; the elected one is the first front cover (type 3), else the first picture. Its anchor, stored length, type, MIME and coding (2.6.4) are recorded; its image is never decoded or kept by the scan (the device reads through its bytes only inside a v2.2/2.3 tag under tag-level unsynchronisation, where the frame after it is found no other way: 3.3.8). |

**The number rule** (both producers, so the f32 of the ESP32's FPU, the
f64 of Rust and a JS `Math.round` can't disagree):

1. Trim ASCII whitespace (space, tab, LF, FF and CR: Rust's
   `is_ascii_whitespace`, so not VT); for a gain, strip one trailing `dB`
   in any ASCII case, and trim again.
2. The rest MUST match `[+-]?([0-9]+(\.[0-9]*)?|\.[0-9]+)`: no exponent,
   no hex, no `inf` or `nan`, no unit or other text after it. Otherwise
   the field is absent.
3. **Exact decimal arithmetic, no floating point:** keep the integer
   digits and as many fraction digits as the scale needs (none for a
   BPM, 2 for a gain, 4 for a peak), padding with zeros; if the next
   digit is 5 or more, add one to the magnitude (half away from zero);
   apply the sign.
4. A result outside the field's range is absent (a peak saturates
   instead). An implementation stops accumulating digits once the value
   is out of range, so a long digit string can't overflow it.

So `1.005 dB` is 101, where f64 arithmetic gives 100.49999… and 100;
2.18 lists the vectors.

### 5.4 What the builder shows (the election)

These are the device's Stage A rules; a terminal view of a card would use
the same.

**Text helpers** (mStream's `src/db/name-key.js`):

- **nameKey:** collapse whitespace runs to one space and trim; fold the
  Unicode quotes (`‘ ’ ‚ ‛ ′` to `'`, `“ ” „ ‟ ″` to `"`) and dashes
  (`‐ ‑ ‒ – — ― −` to `-`); lowercase. No accent folding.
  - **Pinned for the contract** (DJRW's artistKey and the same-song rule,
    2.13.2, are written by one side and compared by the other):
    whitespace is the Unicode `White_Space` property (Rust's
    `char::is_whitespace`); lowercase is the Unicode default full
    lowercase mapping with `Final_Sigma` (Rust's `str::to_lowercase`).
    So `ΣΟΦΙΑΣ` gives `σοφιας`, where a per-character `towlower` gives
    `σοφιασ`. mStream's `name-key.js` notes the gap between its own JS
    and Rust engines (U+FEFF, a few case mappings); the contract follows
    the Rust side.
- **orderName:** the nameKey of the sort tag when there is one, else of
  the name, minus one leading article from `the, el, la, los, las, le,
  les` followed by a space. The device's `textfold::compareSorted` already
  sorts this way, with accents folded too.

**Per track:**

- **Title:** the record's, else the file name's (today's parse).
- **Artist display:** the record's artist values trimmed; one value as is;
  several deduplicated by nameKey (the first kept) and joined with ", "
  (mStream's `credit_display`). Shown where it differs from the album's
  artist line.
- **Disc:** the record's, else the file name's (`1-01`), else the disc
  subfolder's rank as today; missing counts as 1.
- **Number:** the record's, else the file name's.
- **Length:** the record's durationMs, until the open says better.

**Order within an album:** disc, then folder rank, then number (**missing
last**, as mStream's `track-order.js`; today a missing number sorts
first), then the file name.

**Per album** (still keyed by folder):

- **Name:** the most common album value among its tracks' records (exact
  bytes); ties to the smallest by bytes (mStream's tie rule); none: the
  folder name.
- **Year:** the most common year; ties to the earliest.
- **Artist line:** the most common album-artist display; else "Various
  Artists" when any track says compilation; else the most common track
  artist display (mStream's chain, `artist-extraction.js`); else the
  folder.
- **Discs:** the highest disc number; "Disc 2" dividers only when over 1.
- A vote counts over the album's run of tracks in a fixed scratch of 512
  candidates; a longer album votes on its first 512.

**Per artist** (still the artist folder in Stage A): the most common
album-artist display (else the track artist display) among its tracks,
when `textfold::sameName()` matches it to the folder name loosely (case,
accents, the FAT-illegal characters, one leading "The"); otherwise the
folder name. That fixes the about 9% of artist folders that differ from
the tag only in case or punctuation (metascan section 4) without
regrouping.

**Views:** artists and albums A-Z by orderName; **an artist's albums
newest first** (year descending, no year last, then name); the Folders
view raw; no Genres view (the records keep the genres, so one later is a
rebuild).

**For later (Stage B and AutoDJ):**

- **Genres:** mStream's list is the first genre item split on `,`, `;`
  and `/`, each trimmed, empties dropped.
- **Credits and the primary artist:** several artist values are taken
  verbatim; one value is split, ignoring ASCII case, on `" / "`,
  `" feat. "`, `" feat "`, `" ft. "`, `" ft "`, `"; "`, protecting the
  server's split exceptions (A3; none by default); then deduplicated by
  nameKey. The primary artist is the first credit.
- **The same song** (AutoDJ's rule, 2.13.2): equal
  `nameKey(artist display) + "|" + nameKey(title)`.

### 5.5 Where the prototype parser must change (`metascan/parsing/TagScan`)

1. **Precedence:** 5.1 (ID3v2, else ID3v1 before APE, with the ID3v1
   fill), not "ID3v2 > APE > ID3v1, filling only empty fields", and not
   the research's "never merged" (mStream's code fills from ID3v1).
2. **Several values:** lists (2.3.6), not one value joined with "; ".
3. **Latin-1:** ISO-8859-1 in the record; the research's cp1252 fix moves
   to drawing.
4. **Invalid UTF-8:** U+FFFD, not re-read as Latin-1.
5. **TCON with several values:** every item kept (the research's
   "17\0Pop" fix stands); the first-item rule is the election's (5.4).
6. **Year:** four digits after whitespace, Year before RecordingDate.
7. **Compilation:** `1`/`true` and `0`/`false` only.
8. **BPM:** rounded, 20-300; key text through the Camelot table.
9. **FLAC:** a front ID3v2 only when there is no comment block.
10. **More fields:** composer, the sort fields, the recording id, the key,
    the album gain and the peaks; per-field buffers sized for 2.3.6's
    limits (the prototype keeps 159 bytes per field).
11. **The research's other fixes stand:** duplicate values across key
    aliases, v2.4 tag-level unsynchronisation, the firmware's own MP3
    duration helpers, an ASan/libFuzzer pass where a Linux toolchain is
    available.

---

## 6. Milestones: NOW (host-only) and LATER (the Core2)

Effort is in agent-days (ESTIMATED), with the project's usual review and
fix rounds and the doc updates included, and the time spent waiting for
the device excluded. Section 10 maps these to the research's M0-M8.

### 6.1 NOW: the player, host-only (about 21.5-29.5 days)

All of it is `lib/core` code with Unity tests under `pio test -e native`,
or firmware glue that is built (`pio run -e core2`, with the IRAM
`cache_guard` check) but not flashed.

| # | Work | Days | Host proof |
|---|---|---|---|
| N1 | **Built.** **The contract kit** (`lib/core/CardContract`): CRC-32, FNV-1a 64, qfp, FAT time and the skew rule; MSMF, MPTG, MPDJ and MSPD readers and writers; the root election; `device.txt`; 2.4.3's structural checks; the fixtures of 2.17 (2.18's vectors, the JSON library descriptions and their golden files), frozen for the terminal's tests | 2.5-3 | Round trips; truncation at every byte and a flipped bit per section give "absent"; each structural check broken under valid CRCs gives "absent" (no endless loop); a newer major is absent, a newer minor reads; the golden bytes |
| N2 | **Built.** **`LibraryIndex` v6 and `LibraryBuilder`**: Stage A's election (5.4), the merge (2.9), exact sizing, the inputs, LIBR roots | 3.5-4.5 | `test_library_index` extended; `LibrarySynth` with synthetic tags at the measured disagreement rates; 20k memory and build-peak asserts (walked, and T listing after a transfer); the same files from T and from D build byte-identical indexes apart from the sources and lengths within a second |
| N3 | **Built.** **The queue's remap through `queue.txt`**; `QueueModel::release()` and the exact trim | 1-1.5 | `test_queue`: a 20k remap within budget (since the queue's cap, a full queue of 5,000 from a 20k library); shuffled; the current track gone; the resume point carried; the remap after a rebuild or a boot with no library, and after a cleared queue (the file read back from its own line) |
| N4 | **Built.** **`TagStore`**: D with its device sections, `tags.jnl` (sorted chunks), `walk.jnl`, the streaming k-way compaction, recovery, the cut-rename rule (2.12.6) | 2-2.5 | A power cut injected at every write, sync, remove and rename, a rename cut between its two directory writes included; the compaction's PSRAM bounded whatever the journal holds |
| N5 | **Built.** **`CardWalk`**: the lister interface, the canonical sort (with its passes for big folders), the digests, T's freshness (the skew, Doubtful entries through `walk.jnl`, qfp, confirmations) | 2-2.5 | Fake FAT trees: shuffled order, a 3,000-file folder through a small scratch, a retag at the same size, a renamed folder, a deleted album, every stamp shifted an hour, three files shifted, invalid and zero stamps |
| N6 | **Built.** **`TagScan`, the production port** with part 5's rules; the synthetic parity corpus (2.17, item 3) | 3-4 | `test_tag_scan`: the corpus and the crafted edge files (`test/fixtures/tags`, `tools/tag_corpus.py`) field for field against the lofty reference (`tools/tagref`) at every buffer size; the anchors read back; generated files past the corpus's sizes and the read budget; truncation, mutation and generated fuzz passes (ASan only where a Linux toolchain is: a review ran one); a random differential against the reference (3.3.8) |
| N7 | **Built.** **`ScanScheduler`** and the `LibraryWrite` blocker | 1-1.5 | Like `test_idle_policy` (3.3.9) |
| N8 | **Built.** **`SectorCache`.** Optional: a host FatFs model (vendored FatFs on a RAM disk) counting sector reads per walk and per open on a 20k tree of the user's shape (part 7, U14: vendoring is a download; the user said yes). Both built (3.2.7) | 1 (+1) | LRU, bypass, write invalidation, a random model check (`test_sector_cache`); the model (`test_fat_model`, `tools/fatmodel.py`) checks metascan's 56 sectors per open before L0: 58.0 on N11's card, 3.5 with the cache's 256 sectors |
| N9 | **Built.** **The catalog, the UI and the texts**: `TrackCatalog`, `LibraryPage` rows, `UiText`, `SleepTimer`'s `kLoose`, the console's `g*` commands (3.7) | 1.5-2 | `test_ui_library`, `test_sleep_timer` |
| N10 | **Built.** **Firmware glue, built and not flashed** (3.8): the FatFs lister, the diskio wrapper, the card worker, streamed JPEG input, transfer thumbnails, `device.txt`; with them the boot of 3.2.2, the walk, the compactions and the scan under N7's scheduler, the status line and toasts, and a first update step on the loop | 2-3 | `pio run -e core2` and `core2-dio` with every guard (`cache_guard`, `flash_guard`); `test_card_jobs`: the boot's table, the root, the jobs on fake cards (a hand-filled card walked, scanned and built; a transfer card; a bad T; a full journal) |
| N11 | **Built.** **A synthetic big card** (`tools/synthcard.py`): about 20k tiny tagged MP3, FLAC and Opus stubs in the user's shape, with no real names, for L0 without the real library (the user writes it to a card: 6.3's C2) | 0.5-1 | Its own tag dump through N6: `tools/test_synthcard.py` (27 tests: the shape statistics, determinism, stubs read back by its own parsers, by ffprobe and by the repo's through `tools/synthcard_probe.cpp`, N6's TagScan among them; `--copy-to`'s refusals) |
| N12 | **Built.** **`LibraryUpdate`**, the boot decision and the update step as a portable state machine (3.2.2, 3.4.2): the decision table with `library.tmp` recovery and the build-at-boot marker, the safe point, the memory check and deferral, the build as a card-worker job with the scan and compaction paused until the save, the fence on the loop's readers, Thumbs' pools and the queue released and restored, the restart from D alone; and its glue in `Library.cpp`, `main.cpp` and the UI's "Updating library" state, built and not flashed (N10 built the decision table, the marker, a first safe point and memory check, and the update step on the loop: 3.8). With it the Output tab's Library row and its Rescan button (3.3.6), and the compaction's 13 KB stack frame cut to 1 KB (3.9) | 1.5-2.5 | `test_library_update`, like `test_idle_policy`: every row of 3.2.2, `library.tmp` after a cut, a deferral then a boot that builds, a track end near the safe point, a compaction request during a build, a bad T found at the end of a build, a power cut at every step (each way a card comes back), the fence at every read and allocation of the build, a card that fails mid-build, the worker's task before the fence, the marker's build out of PSRAM; `test_gapless_player`: a track that ends inside the fence starts nothing, a join inside it is kept, the queue's edits refused; `test_queue`: a queue that can't be carried (3.9) |
| | **Total** | **about 21.5-29.5** | |

**Order:** N1, then N2 and N3: they fix the shared format (which unblocks
the terminal's tests) and the builder both producers feed. Then N4, N5
and N6 in parallel; then N7-N10 and N12; N11 before the device session.

### 6.2 NOW: the transfer software (mstream-terminal; a proposal for its team)

All testable on a PC with a folder standing in for the card (or a FAT32
card in a reader), and an e2e leg against a canned server. If the contract
is adopted as written:

| # | Work | Days |
|---|---|---|
| S1 | Client methods for `sync/manifest`, `metadata/batch` and `local/embeddings`; card detection (removable FAT32 only; refuse exFAT, NTFS and GPT, saying why) | 1 |
| S2 | The path rules (2.8.3), the per-OS names (2.8.5), the planner (2.10.2: pairing, per-component matching, moves, the guards), the download, conversion and read-back pipeline, the FAT-time recipes (2.3.4), the server key (2.5.1) | 3-3.5 |
| S3 | The reference reader: lofty 0.25 with part 5's rules ported from the rust-parser (GPL-3.0 to GPL-3.0-only is fine) | 1-1.5 |
| S4 | The MPTG, MSMF and MSPD writers, generations, checkpoints, recovery and the cut-rename check (2.12), 2.4.3's checks in its readers, against the shared fixtures | 2-2.5 |
| S5 | Covers: the folder-first source (2.14.2) and MPTH thumbnails with the `image` crate it ships | 1 |
| S6 | AutoDJ: embeddings, an exact cosine top-K over the selection only (blocks across threads, no new crate), the MPDJ writer | 1-1.5 |
| S7 | The page (a `mstream-player device card` command, later a page in the GUI's MP3 Player tab) and the e2e leg | 1.5-2.5 |
| | **Total** | **about 10.5-13.5** |

**Optional mStream work** (another repo, not this run): A1-A6, about
1.5-2 days; A8 2-3; A9 1-2.

**Outside this plan's totals:** the device's AutoDJ engine (the autodj
research's M2, 2-3 days host-tested, and M3, 1.5-2 days with the device);
the embedded-cover decode beyond JPEG (PNG, progressive: metascan's M6).

### 6.3 LATER: needs the Core2 (about 4-7 days, one batch; flashing needs the user's go-ahead)

| # | Work | Days | Device checks (console lines) |
|---|---|---|---|
| L0 | **The M0 bench** on a full FAT32 card (the real library, or N11's) | 0.5-1 | ms per sector; open time at entry 1, 350 and 700 of a 705-entry folder (N11's probes: 4, 55 and 108 card reads in the model, 3.2.7); the stock walk against the 89-148 s model; PSRAM free with the UI up |
| L1 | **The sector cache on** | 1-1.5 | A write soak with the cache on (resume saves, thumbnails, the queue, `library.idx`); the SD write bench unchanged; a remount |
| L2 | **The boot and the validation walk** | 0.5-1 | Browsable in under 3.5 s at 20k; the walk about 10 s; 0 underruns over MP3, FLAC and Bluetooth during the walk |
| L3 | **The scanner** | 1-1.5 | Per-file and full-scan times idle and playing, and on the Dance page; 0 underruns; a reboot mid-scan resumes; the stack's high-water mark; the lowest internal RAM in Bluetooth mode; the loop's `pass_max` during a scan; the battery percent |
| L4 | **The update step at 20k** | 0.5-1 | The PSRAM peak and the largest block; the build's time on the card worker and the fence's (against 3.4.2's 11-14 s: since N12 the loop stays live through it) and the save; a deferral and the boot that builds; the queue, the resume point and a gapless advance survive; the card worker's stack high-water mark during a build (3.9's estimate: about 4.5 KB used at worst); a track that ends inside the fence starts nothing |
| L5 | **The UI at 20k** | 0.5-1 | Scroll smoothness; the status line's cost; streamed covers; the loop's `pass_max` and touch latency while the scan runs |
| | **Total** | **about 4-7** | |

Then, when the parts they need exist:

| # | Work | Days | Device checks |
|---|---|---|---|
| L6 | **Embedded JPEG covers** (2.14.3, step 3; most of it is written in NOW) | 1.5-2 | Decode times idle and during MP3 and Bluetooth; 0 underruns; the worker's stack and internal RAM unchanged |
| L7 | **End to end:** a card filled by the terminal on a PC, then booted | 0.5-1 | No scan of the software's files; the index equal to the host build's from the same card image; the listener's own album scanned; a pulled-card recovery |

**Needs a card, not the Core2** (can run before the Core2 is free, if
the user prepares the card and the machines):

- **C1:** the FAT-time recipes (2.3.4) on a real FAT32 card in a Windows,
  a Linux and a macOS reader, including a DST change; and the names
  (2.8.5): on each OS, create a non-ASCII name (`Café`, an NFD spelling
  made on Linux, a case-only rename), list it through the software's
  rule, and compare it with the bytes the card stores, read by a host
  tool that decodes the directory entries as FatFs does: 0.5 day per
  OS.
- **C2:** N11's synthetic 20k card written by the user, ready for L0.

#### 6.3.1 The runbook (N10's console lines)

What the batch runs, in order, and what each line should say. The
expected figures are this document's (ESTIMATED unless marked); a line
that differs by more than the range is a finding to record next to it.
Serial at 115200 (the console of README); commands end with Enter.

**Before.** The core2 (QIO) build of `feature/metadata` at N10's commit or
later, flashed with the user's go-ahead. The card: N11's synthetic 20k
card (`tools/synthcard.py`, seed 1: no `/.mstream`, every file the
listener's), or the real library. The first boot of this firmware on a
card walks /music and saves; the console's first lines say so, in this
order:

- `[storage] SD card on FatFs drive 0; the sector cache: on (256 sectors,
  135168 B of PSRAM)`
- `[lib] /music walked: N files in M ms (add A ms, finish F ms)`
- `[lib] /.player/device.txt written (firmware 0.7.0-...)`
- `[lib] the card: no transfer data (none on the card); D none (0 records,
  0 journal chunks), read in R ms + D ms`
- `[lib] library.idx outdated: walk /music (an older version's, and no
  records) in N ms (header H ms)` on a card 0.7.0 used (its
  `library.idx` is v5); `[lib] library.idx missing: walk /music (none
  yet, and no records) in N ms (header H ms)` on N11's fresh card. N is
  the walk's time (the line above's M).
- `[lib] N tracks, A artists, B albums; P B of PSRAM; browsable M ms after
  the mount; internal RAM ...`

**L0, the bench** (the stock driver; 3.2.1, 3.2.7). Playback stopped (the
decoder's reads would count):

1. `gl`. `[bench] the card: 200 single-sector reads over the card, mean X
   ms`: 0.6-1.0 ms (3.2.1's calibration). `[bench] /music has N entries`
   (names: N11's card about 705 artists, the user's 705).
2. The three opens, `[bench] the open under entry K (probe P, 3 levels):
   uncached A reads in T ms; cached cold B in T ms, warm C in T ms` (the
   first file of the first folder of the artist at /music's entry 1, 353
   and 703: on N11's card a plain MP3 album, the summary's probes): A = 4,
   55 and 108 (3.2.7's model, give or take the album's own folder
   sectors), B about A (the cache empty), C about 1-3 (the artist's and
   the album's own folder sectors); T uncached about A x the per-sector
   time. These are FatFs lookups (`f_stat`). Whether the VFS's open of a
   track is one lookup or two (its `fstat()`, 3.2.7): with the card
   worker idle (after the first boot's scan, "Library updated", and `gs`
   saying `waiting for nothing`: the walk, the scan and covers read the
   card in the background, and only `gl`/`glw` wait for them), `gc0` (the
   cache off, the counts from zero), play the probe's first track from the
   Folders tab (no covers), `gc`: the card's reads of one sector against A
   (one lookup, plus a few for the file's FAT and first partial sector) or
   twice A.
3. `glw` (minutes at 20k): `[bench] the stock (forEachFile) walk,
   uncached: 19,410 files ...`: about 153,000 card reads and 89-148 s
   (3.2.7's nested walk, 153,241); 3.2.3's (CardWalk over FatFs) uncached
   about 150,742 reads (90-151 s); cached about 7,300 (9.4-12.3 s at 35 µs
   a hit). The two cached figures are what the device keeps.
4. PSRAM free with the UI up: `gl`'s last line (`[bench] PSRAM free ...`)
   or About: 3.5's 2.77-2.90 MB less the cache's 0.13 MB at 77 tracks;
   record it at 20k for 3.5's table.

**L1, the cache on** (3.2.4, 3.2.7; risk 3). `gc2` (every hit checked
against the card), then a write soak of 30 minutes or more: play with
skips and seeks (the resume point's NVS and `queue.txt` writes), queue
edits, `uiT` (every cover decoded again, its card copy rewritten), `gb`
(library.idx and a compaction), `gr` (the journal, then a compaction at
the next epoch). Then:

- `gc`: `[cache] verify: N hits checked, 0 STALE` (any STALE fails L1: the
  sector's number follows); `[cache] ... writes W (S sectors, U cached
  sectors refreshed, 0 failed)`; the hit rate during the soak.
- The firmware has no SD write bench: record the saves' times instead
  (`g`'s `[index] the card's boot: ... save X ms`, the queue's `q` line)
  with `gc0` and with `gc1`; they should agree within noise.
- A remount: restart. `[lib] library.idx matches the card: load (its
  inputs are the card's)` and the library as before (`g`); `gs` reads D
  whole (`D /.player/tags.bin: N records ...`).
- FatFs's own remount (the card pulled and put back, the player paused,
  `gc2` on): the next access mounts again (`gc`'s `N mounts` higher, one
  for each try while it was out, the cache emptied), the card's tracks play as before, and `gc` says 0
  STALE after more plays and queue edits. While it is out: no update
  step runs (`[card] the scan's end: its rest stopped on a read error`,
  or `[lib] the update step: the card doesn't answer (...)`, before any
  fence), the library stays,
  and the loop isn't held pass after pass (a refused chunk is offered
  again every 30 s).

**L2, the boot and the validation walk** (3.2.2, 3.2.3, 3.2.5):

1. An unchanged card: `[lib] library.idx matches the card: load (...) in
   N ms` (5 ms at 77 tracks MEASURED before; 1.1-1.5 s at 20k) and
   `browsable N ms after the mount` (under 0.5 s at 77; under 3.5 s at
   20k).
2. About 2 s after the UI's first frame (the Library's status line says
   "Checking the card…"): `[card] the walk: F folders (L listings, 0
   merged), A audio ...; 0 added, 0 changed, 0 gone; ...; S steps in M ms`:
   M about 10 s at 20k with the cache (under 0.05 s at 77); `gs`'s `[card]
   walk: S steps, mean m ms, longest l ms` (a step is a folder: l under
   100 ms wanted).
3. During the walk, play MP3, then FLAC, then on the headphones: the
   `[stats]` line's `underruns=` doesn't move; `gs`'s `[card] waited (s):`
   shows what the walk yielded to.
4. A hand-copied album added on a PC (12 files): the walk says `12
   added`, the toast "Found 12 new tracks", the status line "Reading tags
   0 / 12" then "Updating library…", and `[card] the update step (the
   scan's end): the library is rebuilt`; the album shows with its tags.
5. 200 files or more added: `[card] the update step is asked (the walk
   found new files)` right after the walk (U11): the files show by their
   names first, with their tags after the scan.

**L3, the scanner** (3.3; risk 7). N11's card from a fresh `/.player`
(delete it on a PC: every file the scan's). `gs`'s waits, steps, stack
and internal RAM are totals since the boot or the last `gs0`: type `gs0`
as each condition starts (idle, MP3, FLAC, the headphones, the Dance
page) and `gs` at its end.

1. The first boot walks /music (the line above), the walk adds every file
   (`19,410 added`, no toast: none is new to the index), and the scan
   reads them: "Reading tags N / 19,410". `gs`: `[card] scan: S steps,
   mean m ms, longest l ms`: m about 8 ms idle with the cache (7.5-8.7),
   about 18 ms while playing; l under 100 ms. The whole scan (the status
   line's start to "Library updated"): 2.5-2.9 min idle, 3-6 min playing.
2. The same while playing MP3, FLAC and on the headphones, and on the
   Dance page: `underruns=` doesn't move; `gs`'s waits show the ring,
   track changes, decode passes.
3. A restart mid-scan: `gs`'s rows count `tags.bin` alone, so before the
   first compaction (about 2,400 files) D's Pending rows look unchanged;
   compare `gs`'s `[tags]   journals: tags.jnl N B` before and after the
   restart (kept), and `[card] jobs: ... N files read` once the scan ends
   after it: the files the first session hadn't recorded, and at most the
   last 100 (or 5 s) again, plus the loop sources' tracks (the playing
   track, the queue's next 200, the Library tab's page) whose records the
   journal already has: the index loaded says Pending as `library.idx`
   saved it (the rest's View skips them). The status line's M after the restart is that
   index's Pending count (every file, on a walked card's first index), not
   the files left.
4. The worker's stack: `gs`'s `[card] the worker: ... its 6 KB stack's
   least left N B (this life L B)` (the least over every life of the
   task since `gs0`: 1 KB left at least; less is a finding: the stack
   grows before release). Internal RAM: the same line's `lowest X B while
   a step ran` in Bluetooth mode, against the 50 KB the soaks kept.
5. The loop: the `[stats]` line's `pass_max=` and the scroll lines' frame
   times during the scan (L5's too).
6. The battery: on battery below 10%, `[card] the battery is below 10%:
   the scan waits for USB or 15%`, and `gs` says `waiting for the battery
   (below the floor)`.
7. Compactions: `[card] compaction done: N records ...` at most every
   3,200 files (`append()` refuses a 33rd chunk of at most 100 files),
   about every 2,400 (`tags.jnl` reaching 512 KB first: 8 in 20k on the
   host, 3.3.7); `gs`'s `[card] compaction: S steps, mean m ms, longest
   l ms`: the last one 10-16 s at 20k (3.3.7).

**L4, the update step at 20k** (3.4.2, 3.9; risk 8). With the queue full
(Shuffle all: 5,000) and an MP3 playing. Since N12 the build and the save
are the card worker's and the loop stays live through them (at N10 the
build held it): the build's length, the fence's and the loop's own
figures through it are what L4 records.

1. `gb` (or the scan's end): `[card] the update step is asked (gb)`, then
   `[lib] the update step (gb): the fence is up; the build on the card
   worker (the loop goes on); PSRAM free X B before, Y B now` (Y over X by
   about the queue's entries (up to 120 KB), Thumbs' pools' 315 KB, the
   scan's memory, and the old index less its track table, which is kept
   for the build: about 1.15 MB of the index's 1.8 MB at 20k),
   then `[card] the update step (gb): the library is rebuilt (its save
   next, on the card worker)`, `[lib] the update step: built in N ms on
   the card worker (the loop live; the fence up F ms); PSRAM free X B
   before, Y B after, lowest Z B` and `[lib] built from the records: ...
   peak P B` (3.4.4: 1.99 MB at 20k). N against 3.4.2's reads, adds and
   finish (about 7-9 s idle, 9.5-11 s while an MP3 plays; the build now
   shares the CPU with the loop: record both); F about N and a pass;
   over 16 s grows the safe point; Z the PSRAM's lowest since boot (3.5's
   margin about 0.7 MB). Record `[lib] the update step waits for the next
   boot (...)` if it comes instead: its reason says which test failed
   (the room, or the track table fitting neither the old one's block nor
   the largest free one).
2. The loop through the fence: touch, Now Playing's time and names (the
   held copy), the volume and pause and resume act; the lists say
   "Updating the library…" and the status line "Updating library…"; a
   skip (next, prev, shuffle) gets the note "Updating the library: a
   moment"; the Queue tab offers no Edit (a selection open at the fence
   closes); the seek bar shows no knob; the
   `[stats]` line's `pass_max=` against the same without a build; 0
   underruns.
3. The save: `[lib] the update step: library.idx saved in X ms on the
   card worker` (2-4 s at 20k); `g`'s `[index] the last update step
   (gb): built in ... the fence up ... saved in ...`.
4. The queue survives: `[queue] after the rebuild, carried through
   queue.txt: 5000 of 5000 tracks still there, at K` (and `(its start
   point kept)` when paused with a resume point); the playing track plays
   on and the next one joins gaplessly (`G`'s counters: no cut, no
   restart).
5. A deferral: `gb!` (the update step with its memory check made to
   fail; a short PSRAM is hard to make on purpose: the room test fails
   under about 0.24 x the index's bytes plus 106 KB free, less what the
   step frees besides the index, the queue's and Thumbs' pools' 315 KB,
   and the table test depends on the heap's blocks): `[lib] the update
   step waits for the next boot (gb! asked for the deferral)` and the
   toast "Library updates at next boot"; restart: `[lib] library.idx
   matches the card, the build-at-boot marker set: build from the records
   (a build was deferred to this boot)`, the marker gone after.
6. The worker's stack during a build: `gs0` before `gb`, `gs` after the
   save: `[card] the worker: ... its 6 KB stack's least left N B` against
   3.9's estimate (about 4.5 KB used at worst, 1.5 KB left); less than
   1 KB left is a finding. Then the same after a compaction (`gr`, then
   the scan's end): 3.9's fix of its 13 KB frame.
7. A track that ends inside the fence (hearing safety): pause a track
   with about 5 s left, `gb`, and resume it while the fence is up. Gapless
   on: it joins the next, which plays on, and after the fence `G` counts
   the join adopted, none restarted. `G0` (gapless off), the same: the
   track ends, silence, and after the fence `[queue] the track ended while
   the library updated: the next one waits, paused (nothing starts by
   itself)`; Now Playing shows the next track paused at 0:00; B plays it.

**L5, the UI at 20k** (3.3.6, 3.7):

1. Scroll the Artists and Albums lists while the scan runs: the `[ui]
   scroll:` lines (fps, draw mean and max, ring min, `underruns +0`).
2. The status line: drawn at most twice a second; the `ui` report's fps
   with the line up and with it gone (3.3.6: 10 Hz cost 5%).
3. Covers: `[thumb] <path>: WxH at 1/n, 40 + 96 px in N ms (header and
   reads R, ...; K KB streamed)`; a cover over 2 MB decodes now; on a
   transfer card `ui`'s `[thumb] made: ... N transfer thumbnails`.
4. N9's checks: the "Disc N" divider's look (Bold accent text at y 25, a
   hairline at y 39), Now Playing's "Album · 2001" fit, `gs` and `gt` on a
   real card, the Album page's dividers keeping the playing row's reveal
   and Play from a track.
5. Touch latency and the `[stats]` line's `pass_max=` while the scan
   runs, against the same without it (`gc` changes nothing here; stop the
   scan by unplugging USB below the battery floor, or compare with the
   card's second boot, when nothing is Pending).

---

## 7. Open questions

### 7.1 For the user

The user answered U8, U11, U12, U13 and U14 on 2026-10-07 (marked
**Decided** below), and part of U1.

- **U1. One card for the whole library?** If about 19,400 files will go on
  one card, can you prepare a FAT32 card for L0: the real library, or
  N11's synthetic one written from the PC? **Answered in part
  (2026-10-07):** a spare card is ready for N11's synthetic 20k card and
  later tests.
- **U2. The card's layout.** Mirror the server's paths minus the vpath
  (proposed: Artist/Album stay at depths 1 and 2), or keep
  `/music/<vpath>/` folders as declared roots? With several libraries
  selected, merge same-named artist folders, or keep them apart?
- **U3. The conversion target** for files the player can't play (not MP3,
  FLAC or Opus; over 48 kHz; Opus with more than 2 channels): Opus 192k
  (works today, sample-exact and gapless on the player) or MP3 320k at
  44.1 kHz (needs A5)?
- **U4. A song the listener deleted from the card:** copy it again at the
  next run (a strict mirror), or treat it as removed by the listener?
  And one the listener **moved** on a PC (found elsewhere with the same
  size and qfp, 2.10.2): follow the move and keep the file where the
  listener put it (proposed), ask, or move it back?
- **U5. A software-owned file the listener edited:** keep it and report it
  (proposed), or overwrite it so the card mirrors the server?
- **U6. A card with music but no ledger:** adopt the files that equal the
  selection (same path and size, qfp checked) after asking, or always
  write beside them?
- **U7. A card filled from another mStream server**, or one whose server
  can't be told (2.11: different or unknown), or a run that would delete
  most of the ledger (2.10.2, step 5): refuse, ask (proposed), or take it
  over?
- **U8. Rescan tags:** should it override transfer records? Today a
  matching transfer record wins, so a retag that kept both the size and
  the time of a software-owned file stays invisible until the next
  transfer. **Decided (2026-10-07): no, the default stands:** a transfer
  record wins while it still matches (2.9;
  `LibraryBuilder::kTransferBeatsRescan`).
- **U9. Covers.** (a) The folder ranking: the device's `cover > folder >
  front > the largest other .jpg`, or mStream's `folder > cover > album >
  front`, then PNGs, then by name? (b) A cover the listener adds to a
  transfer-filled album wins over the transfer thumbnail (proposed): right?
  (c) Should the software also write a baseline `cover.jpg` into album
  folders it owns that have none, so other players show art? It would be
  an owned non-audio record.
- **U10. Several genres:** mStream's two engines likely disagree (Rust:
  the first item; JS: all). The records keep all items; should the device
  follow the Rust engine when a Genres view comes, or should mStream be
  fixed to keep all?
- **U11. New hand-copied files:** show them at once with file names (an
  extra update step, about 9-12 s at 20k, 11-14 s while an MP3 plays:
  3.4.2), or only once their tags are read? **Decided (2026-10-07): at
  once, with their file names** (3.3.3's build right after the walk; the
  tags follow at the scan's end).
- **U12. If PSRAM is short at 20k** with a whole-library queue and
  AutoDJ: drop the undo snapshot for queues over about 5,000 entries
  first, or keep file names on the card instead of in PSRAM? **Replaced
  (2026-10-07) by a cap on the queue: 5,000 entries.** Shuffle all and
  Play all on a bigger library take a random 5,000 (shuffle on) or the
  first 5,000 in order (shuffle off); an add past the cap takes as many
  as fit and is refused with a toast, "The queue holds 5,000 tracks",
  when none do (adding what fits was this design's call:
  QUEUE-MODES.md 15.3); the undo is always kept. Built and host-tested
  (QUEUE-MODES.md section 15); 3.5 has the budget. The file-names lever
  stays in 3.5's table, unused.
- **U13. The scan's battery floor:** 10% (proposed), lower, or a setting?
  **Decided (2026-10-07): 10%** (3.3.5: paused below 10% off USB,
  resumed on USB or above 15%).
- **U14. May a host test vendor FatFs** (a download) to count sector reads
  on a synthetic 20k tree before L0? **Decided (2026-10-07): yes** (N8's
  optional model). Built: R0.15 from elm-chan.org, in `test/support/fatfs`
  for the host only (3.2.7).
- **U15. AutoDJ's table:** K = 100 (about 6 MB per 20k tracks on the
  card)? Rows only for embedded tracks, or also rows with filter data
  only (BPM, key) for tracks not yet embedded? And random with filters
  (2.13.4): enough with the DJRW rows' BPM and key, or worth about 80 KB
  of PSRAM at 20k for a per-track filter block that covers the
  listener's own files too?
- **U16. ReplayGain album gain:** will the device apply it? If so, A9
  makes mStream agree; the records carry it from the file either way.
- **U17. "No new APIs" for v1** (2026-10-01): still the rule, or may A1-A4
  land alongside?
- **U18. The contract's home and licence:** this file and
  `test/fixtures/card/` in the player repo, mirrored into
  mstream-terminal (proposed), or mStream's docs as well? And a permissive
  licence (CC0) on the fixtures, so all three repos can copy them? The
  player is GPL-3.0-or-later and the terminal GPL-3.0-only, so the
  player's code may flow into the terminal but not back.

### 7.2 For the mstream-terminal design

- **T1. The contract itself:** adopt it, or counter-propose? Above all:
  the root file and generation-named companions (2.5), the 72-byte
  records (2.6.4), the ledger in ORIG (2.6.6), the stage folder and
  `pending.bin` (2.12).
- **T2. lofty 0.25 as the reference reader** (pure Rust, MIT/Apache;
  ESTIMATED +0.5-1.5 MB per binary). The alternative is FROM_API records,
  which can never match a device scan exactly.
- **T3. Where the selection lives:** on the card (`/.mstream/state.json`,
  so any PC with the terminal can continue), in the terminal's config, or
  later on the server so mStream's web panel can show it? The same
  question holds for the list of server keys the listener has called
  this card's server (2.5.1).
- **T4. The read-back:** will the software read names and FAT times back
  from the card with the per-OS recipes (2.3.4, 2.8.5: NFC on macOS, a
  `utf8` mount on Linux), compute path hashes from the names read back,
  never from the names it meant to write, and match its ledger to the
  listing one component at a time (2.10.2)?
- **T5. The checkpoint:** every 500 files or 5 minutes, against rewriting
  a tags file of about 6.9 MB at 20k?
- **T6. Records for the listener's files:** the software could also write
  records for files it finds but didn't write (a card through the terminal
  then needs no device scan at all). Proposed: no, the device keeps them;
  the software's records cover its ledger only.
- **T7. Folder images:** HEAD probes now, A4 later?
- **T8. AutoDJ:** built in the terminal (proposed; no API change) or A8 on
  the server? K, and the same-song rule (2.13.2).
- **T9. Quick Connect tunnels:** fine for the manifest and the
  embeddings, slow for gigabytes of music. Warn, or refuse a transfer over
  a relay?
- **T10. Where it lives in the terminal:** a sibling of the flasher
  (`src/device/`, compiled out of wasm), as a command and a page in the
  MP3 Player tab. The flasher's "SD card untouched" stays true: the
  transfer never uses the serial port. Its "Next: music on the card"
  text, which names `/music/Artist/Album/NN - Title.mp3`, becomes the way
  in; mStream's design cards' tag-derived `NN - Title` renaming is dropped
  in favour of the server's paths (2.8.3).
- **T11. The skew thresholds** (8 files and half of them, quarter hours,
  at most 24 h): checked on real cards (C1)?

---

## 8. Risks

1. **The big-card figures are modelled, not measured.** The per-sector
   latency, the walk and the opens decide how urgent the sector cache is
   and how long scans take. L0 retires it.
2. **PSRAM at 20k** leaves about 0.20 to 0.65 MB (a full queue of 5,000,
   its undo snapshot and AutoDJ, while a card-worker job runs, with the
   sector cache's 128 KB) since the queue's cap; it was about −0.16 to
   0.29 with a whole-library queue. The
   levers are in 3.5; fragmentation can defer a build to the next boot,
   which the build-at-boot marker makes happen.
3. **Sector-cache invalidation bugs would corrupt data.** The host checks
   it under FatFs (3.2.7: the same work with the cache and without it
   leaves the same card); L1's write soak comes before anything else
   ships.
4. **Two implementations of one format** (Rust and C++) can drift: the
   canonical order, path bytes (NFC and NFD, and how each OS lists
   them), string limits, number parsing, the AutoDJ scores. The golden
   files, 2.18's vectors, byte-identical writer tests, 2.4.3's reader
   checks and the read-back rule cover it; the parity corpus covers the
   tag rules; C1 checks each OS's listing on a real card.
5. **mStream's rules move.** The album key, the year rule and the engines
   have changed between schemas. `readRules` versions the records; A3
   would carry the server's rules; the parity corpus pins this version.
6. **PC timestamps.** Time zones, DST and 2 s rounding are absorbed by
   the skew rule and qfp; the worst case is about 2.7 min of checks once.
7. **Long scans during Bluetooth playback** are unmeasured: the ring gate
   and the back-off are the guard, L3 the proof.
8. **A track end during the update step.** The safe-point rule (20 s left,
   no recent seek) covers it, with about 6 s to spare at 20k while an MP3
   plays (3.4.2); L4 measures the pause. Now Playing keeps its copy of
   the names.
9. **Retags the identity can't see:** a retag that keeps the size and the
   time, in the middle of the file. Rescan tags on the device, and the
   software's next run against the server's hashes, catch most.
10. **Licences.** The AutoDJ table is CC BY-NC-SA derived data (2.13.4);
    analysed BPM and key come from an AGPL library on the server, an
    owner question mStream already records; neither is code in the
    firmware.
11. **FAT renames are two directory writes.** A cut between them leaves
    two entries on one chain; both sides check for it before deleting
    anything (2.12.1, 2.12.6), and the software asks for a disk check.

---

## 9. The three lenses, reconciled

Three designs were written for this document (the card contract; the
device; mStream's API and the terminal) and read against the code. Where
they disagreed:

| # | Question | The lenses said | Taken, and why |
|---|---|---|---|
| 1 | How a commit becomes visible | A root file naming generation-numbered companions; a manifest written last over fixed names; `X.new` swapped per file | **The root and generation names** (2.5): it commits the tags, the AutoDJ table and the thumbnails' flags together, and on FAT the root's write is the only rename that matters |
| 2 | The path hash's input | The full card path with `/music`; the path relative to `/music` | **The full path with a literal lowercase `/music`**: the device's thumbnails already hash the index's `"/music/.../cover.jpg"` (`Thumbs.cpp`, `thumbfile::pathHash`) |
| 3 | Where the software's tag fields come from | mStream's API first, lofty for the rest; the card file through lofty | **The card file** (2.7): the manifest's `artist` and `album` are server-wide consensus spellings (`artist-aggregate.js`, `album-aggregate.js`) that a device scan can't reproduce. The API is a flagged fallback (FROM_API) and the source of what only the server knows |
| 4 | Tag precedence | "One tag, never merged" (the research); ID3v1 fills six blank fields, ID3v1 before APE | **mStream's code** (5.1): `rust-parser/src/main.rs` fills title, artist, album, genre, year and track from ID3v1, and lofty orders ID3v2, ID3v1, APE |
| 5 | Latin-1 | cp1252 for 0x80-0x9F; ISO-8859-1 | **ISO-8859-1 in records** (lofty's `latin1_decode`), cp1252 only when drawing |
| 6 | Several values | "; " joined (the prototype); U+001F lists; ", " joined | **U+001F lists in records**, the ", " join in the election (5.4): records keep what the file says |
| 7 | When a transfer record is fresh | Size and time within 2 s, or a fingerprint; ±1 h exactly; the uniform-skew rule | **The skew rule plus qfp** (2.3.4, 2.9), confirmations saved by the device: it covers any constant time-zone error, not only DST |
| 8 | The fingerprint | CRC-32 of the first and last 4 KiB; FNV-1a 64 of the size, head and tail | **qfp** (2.3.5), and it moves from the ledger into RECS so the device can read it |
| 9 | The transfer thumbnail's key | The cover file's path; the album folder's path | **The folder** (2.14.1): no cover file need exist on the card |
| 10 | The transfer thumbnail's source | mStream's art election (embedded first by default); the folder's image first | **Folder first** (2.14.2), the user's rule, so the transfer thumbnail can come first on the device; a cover the listener adds still wins |
| 11 | Does the device pick "the largest .jpg"? | The README says so and the code doesn't (one lens) | **It does**, in `Thumbs.cpp`'s `pickLargest()`, when no well-named cover exists and there is more than one `.jpg`; the index alone records the first |
| 12 | The device's own records | `/.player/scan.bin`; `/.player/tags.bin` | **`tags.bin`** (MPTG source 1, as the research named it), with device-private sections for the listing's status |
| 13 | The canonical order | Paths plus `/` in byte order; pre-order with siblings by name bytes | **Pre-order** (2.6.8): the two differ (`A` against `A B`), and pre-order is what a sorted recursive walk emits |
| 14 | A primary-artist field | Stored by the software; not stored | **Not stored**: it is derived (the split rules and the server's exceptions), so the builder derives it (5.4) |
| 15 | Partial downloads | `.name.part` beside the target; `/.mstream/stage/` | **`stage/`**: nothing half-written ever sits in the listener's folders, and one folder to clean |
| 16 | The generation | u64; u32 with a commit id and a card id | **u32, commitId and cardId** (2.5.2): a restored backup is told apart by the commit id |
| 17 | Normalising tag text | The software writes NFC; nobody normalises | **Nobody**, so two readers of one file write the same bytes; NFC only for the names the software creates |
| 18 | AutoDJ rows | Without an artist key or a licence; with both | **Both** (2.13): the device's artist cooldown needs a key without the tags file, and the data's licence travels with it |
| 19 | BPM's width | u8 capped at 255; u16 × 10 | **bpm10 u16**: mStream accepts 20-300 |
| 20 | `device.txt` | Only the contract lens had it | **Kept** (2.15): the software can't otherwise know what a firmware reads |

---

## 10. The other docs, and sources

### 10.1 Docs to update when this is built

- **README.md:** the library section (tags are read; the transfer
  software; what the scan costs), the cover order.
- **docs/ARCHITECTURE.md:** "Library and queue" (the boot, the walk, the
  builder, `library.idx` v6), "Storage" (the sector cache, `/.player`
  files), and the Roadmap's first three items: the sync now ships
  per-file records (this contract), not mStream-built index files; covers
  and lengths come from it; AutoDJ's table is MPDJ.
- **docs/UI-SPIKE.md:** the per-track memory figures.
- **docs/GAPLESS.md** and **docs/QUEUE-MODES.md:** the update step's safe
  point and the queue's remap through `queue.txt`.
- **mstream-terminal:** its own design doc for the transfer, against this
  contract.

### 10.2 The research's milestones, mapped

| Research (metascan) | Here |
|---|---|
| M0 the bench | L0 (and N11, C2) |
| M1 big-card groundwork | N3, N5, N8, N10; L1, L2 |
| M2 TagScan, Now Playing's tags | N6, N9 (the overlay); L3 |
| M3 the scan engine and tags.bin | N4, N5, N7; L3 |
| M4 the index, Stage A | N2; L4 |
| M5 the UI | N9; L5 |
| M6 embedded covers | L6 (JPEG only) |
| M7 the sync producer | part 2 and S1-S7; 2.17's builder test; L7 |
| M8 Stage B | later; 5.4's "for later" |

### 10.3 Sources

- **The player** at cc13597: `src/app/Library.cpp`, `src/storage/LocalStorage.cpp`,
  `lib/core/LibraryIndex`, `TrackCatalog`, `TrackName`, `QueueStore`,
  `QueueModel`, `ThumbCache` (`thumbfile`), `src/ui/Thumbs.cpp`,
  `OpusOpenCache`, `NvsLayout`, `IdlePolicy`, `CardFormat`, `TrackProgress`,
  `src/main.cpp`.
- **The research** (scratchpad): `metascan/RESEARCH.md` and its prototype
  `metascan/parsing/TagScan.{h,cpp}`; `autodj/RESEARCH.md`.
- **mStream** master 926b97b1: `src/api/sync.js`, `src/api/db.js`,
  `src/api/discovery.js`, `src/api/transcode.js`, `src/api/album-art.js`,
  `src/api/random.js` (`CAMELOT_TO_KEYS`), `src/api/server-info.js`,
  `src/db/artist-aggregate.js`, `src/db/album-aggregate.js`,
  `src/db/name-key.js`, `src/db/track-order.js`,
  `src/db/artist-extraction.js`, `src/state/config.js`,
  `rust-parser/src/main.rs` (the tag selection, the ID3v1 fill, the year,
  BPM, compilation, album-artist, genre and credit rules).
- **lofty** 0.25.1 (the cargo registry): `src/util/text.rs`
  (`latin1_decode`), `src/mpeg/mod.rs` (the MP3 tag order).
- **mstream-terminal** origin/main 2b04dbe: `Cargo.toml` (GPL-3.0-only;
  `image` 0.25, `sha2`), `src/device/page.rs`, `src/dj.rs`, `PLAN.md`;
  `claude/gui-mp3-player`: `docs/ux-contracts/mp3-player-screen.md`.
- **The test vectors** of 2.3 and 2.13.3 were computed for this document
  (a short Python check of CRC-32, FNV-1a 64, qfp, the FAT fields and the
  SHA-256 signature).
