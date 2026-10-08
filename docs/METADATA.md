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
(3.4.4 says what it decided); the firmware builds v6 from its walk alone
until N12 brings the builder in. N3, the queue's remap through `queue.txt`,
is in `lib/core/QueueRemap` (with `QueueModel::release()`, the exact
trim and `queuetext::read()`'s pre-sized blocks), built into
`QueueStore::remap()` and not flashed. The queue's cap of 5,000 tracks
(the user's answer to U12: 3.5, and docs/QUEUE-MODES.md section 15) is
built the same way. Part 2, the card
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
- **A PSRAM sector cache under FatFs** (64 KB) makes the walk and every
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
| v6, hard inputs match, and the build-at-boot marker `/.player/build.req` present (a build was deferred: 3.4.2) | any | Compact the journals if any. **Build from the records** behind the boot screen, on a fresh heap, as in the next row; the marker goes after the save. | The validation walk (3.2.3). |
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

**`lib/core/SectorCache` (portable):** an LRU of single 512 B sectors, 128
entries (64 KB of PSRAM), hashed by LBA. Multi-sector reads (file data)
bypass it; writes go through and invalidate the sectors they cover; it is
cleared at mount.

**Installed (firmware):** a diskio wrapper registered with
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
Opus from the last granule). Pictures are located, never read. The
reading rules are part 5's.

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
  seconds.

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
  transfer).
- **The console:** `g` (today's report plus the scan state), `gs` (the
  scan's status), `gr` (rescan tags), `gr!` (rescan everything, transfer
  files included, as a diagnostic), `gt</music/...>` (one file's records
  from both sources and the winner), `gw` (walk now), `gb` (build now),
  `gv` (Verify: qfp of every software-owned file against T). `g0` stays
  "walk and rebuild".
- **Texts:** the placeholders in `lib/core/UiText.h` and the empty state
  ("Put folders in /music/Artist/Album/") are reworded: tags are read now.

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
  `test_idle_policy`.

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
  shrink hook (`heap_caps_realloc` on the firmware: N12's glue), else by a
  copy as before.
- **Stage A's order inside an album applies to an album with a record**
  (the album flag `kTagged`); an album of path names alone keeps today's
  order (folder, disc, number with none first, name). So a card with no
  records indexes exactly as today, to the byte apart from the version
  and the record sizes: `test_library_index`'s path tests pass unchanged.
  Track flags also say which of the title, the number and the disc are the
  record's (`kTagTitle`, `kTagNumber`, `kTagDisc`).
- **`load()` keeps `Stale`** for a hard-input mismatch, and today's walk
  signature is one of the hard inputs until N12's boot stops computing it.
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
| The sector cache | −0.07 | 3.2.4 |
| `DurationBook` | −0.04, or 0 once lengths come from the index | `lib/core/QueueView.h` |
| The card worker's job, only while one runs (one at a time) | −0.08 to −0.1 | the largest job: the walk (the 64 KB scratch, DFLD's 8 KB buffer, the 2 KB Δ histogram, the journal's write buffer). The scan: its 4 KB buffer, the record, the `FIL`, a 100-record chunk, the resume set (about 60 KB). The compaction: its run buffers (about 60 KB). The doubtful files go to `walk.jnl`, not RAM (3.2.3) |
| **Headroom** | **about 0.26 to 0.71** | positive in the worst case since the queue's cap (it was about −0.1 to 0.35 with a whole-library queue) |

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
  boot builds.
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
applies to any layout shift.

**Card time** at 20k (ESTIMATED): reading T's FOLD, RECS and STRS, about
3.9 MB, takes 2.3-3.3 s, only during a build; checking DJNB's CRC, 6 MB,
takes 3.5-5 s once per commit in the background; an AutoDJ pick reads
one 300 B row (about 3 ms).

### 3.6 What changes, file by file

| File | Change |
|---|---|
| New `lib/core/CardContract` (with `CardContainer`, `CardTags`, `CardManifest`, `CardAutoDj`; built in N1) | The contract kit: CRC-32, FNV-1a 64, qfp, FAT time and the skew rule; MSMF, MPTG, MPDJ and MSPD readers and writers (the device writes only MPTG; the writers serve the host tests and the future sync agent); the root election; `device.txt`; the canonical order; 2.4.3's structural checks. Its golden files are 2.17's, its vectors 2.18's. |
| New `lib/core` modules | `TagStore` (D, `tags.jnl`, `walk.jnl`, the streaming compaction, recovery, the cut-rename rule of 2.12.6); `CardWalk` (an `IDirLister`, the canonical sort with its passes, the digests, T's freshness, the skew, the merge); `SectorCache`; `TagScan` (the production port of the prototype, with part 5's rules); `ScanScheduler`; `LibraryBuilder` (the merge into `LibraryIndex`, part 5's votes, the streamed checks); `LibraryUpdate` (the boot decision and the update step as a state machine: N12). |
| `lib/core/LibraryIndex.{h,cpp}` | v6 records and the header's inputs; `begin(const Sizing&)` with exact counts; `addRecord(path, const TagView&)` next to `addFile()`; Stage A's votes and orders in `buildViews()` (a missing number sorts last, an artist's albums newest first); `readNames()` fills only the fields a record lacks; `kLoose` and the transfer-thumbnail flag; library roots (LIBR), if the vpath layout is chosen. `Load::Stale` no longer happens at boot. |
| `lib/core/TrackCatalog.{h,cpp}` | `title()` the tag's own string or the slice; `artist()` the track artist, else the album's line, else the folder artist; `album()` the display name; `durationHintMs()` the library's length; a one-slot overlay for the playing track's fresh tags (3.3.3). |
| `src/app/Library.{h,cpp}` | The boot decision (3.2.2) replaces `begin()`'s walk, `library.tmp` recovery and the build-at-boot marker included; `rebuild()` becomes the update step, driven by `LibraryUpdate`, its build a card-worker job; the save moves to the card worker; `report()` gains the scan state. |
| `src/app/QueueStore.cpp`, `lib/core/QueueModel` | `remap()` through `queue.txt` (flush, free, rebuild, re-read); the reads pre-size their sinks; `QueueModel::release()` and an exact-size trim. Built (N3). Then the cap of 5,000 entries (`kMaxEntries`, `room()`, `window()`; `queuetext::read()`'s window; the UI's toasts): built, QUEUE-MODES.md section 15. |
| `src/storage/LocalStorage.cpp` | A FatFs lister (`FILINFO`'s size and time); the sector-cache wrapper after `SD.begin()`; `forEachFile` stays for LittleFS and the console. |
| `src/ui/Thumbs.{h,cpp}` | The worker becomes the shared card worker (walk, scan and cover jobs); cover sources in 2.14.3's order, `/.mstream/thumbs` read-only (and `hasCover()` true for an album with the transfer-thumbnail flag); streamed JPEG input. |
| `src/ui/LibraryPage.cpp`, `NowPlayingPage.cpp` | Direct `trackTitle()` reads move to the catalog; rows show a year subtitle, disc dividers and a track-artist subtitle; the status line. Go to artist and album keep the folder entities. |
| `lib/core/SleepTimer.cpp` | End of album tests `kLoose`, not an empty album name. |
| `lib/core/IdlePolicy.{h,cpp}`, `src/main.cpp` | The `LibraryWrite` blocker; the journal flush at shutdown; the boot and `rebuildLibrary()`. |
| `lib/core/NvsLayout.h` | **No change** (`kCurrent` stays 2): the library's state lives on the card and moves with it. AutoDJ's on/off would be schema 3 (autodj research section 7), outside this plan. |
| `lib/core/OpusOpenCache` | **Unchanged:** keyed by path hash and size and checked at the open, so rebuilds don't touch it. |

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
| picture | Every non-empty embedded picture is seen, except a compressed or encrypted ID3v2 frame; the elected one is the first front cover (type 3), else the first picture. Its anchor, stored length, type, MIME and coding (2.6.4) are recorded; its bytes are never read by the scan. |

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
| N4 | **`TagStore`**: D with its device sections, `tags.jnl` (sorted chunks), `walk.jnl`, the streaming k-way compaction, recovery, the cut-rename rule (2.12.6) | 2-2.5 | A power cut injected at every write, sync, remove and rename, a rename cut between its two directory writes included; the compaction's PSRAM bounded whatever the journal holds |
| N5 | **`CardWalk`**: the lister interface, the canonical sort (with its passes for big folders), the digests, T's freshness (the skew, Doubtful entries through `walk.jnl`, qfp, confirmations) | 2-2.5 | Fake FAT trees: shuffled order, a 3,000-file folder through a small scratch, a retag at the same size, a renamed folder, a deleted album, every stamp shifted an hour, three files shifted, invalid and zero stamps |
| N6 | **`TagScan`, the production port** with part 5's rules; the synthetic parity corpus (2.17, item 3) | 3-4 | The corpus and the crafted edge files; the fuzz harness (ASan only if a Linux toolchain is available); parity against a lofty reference (the terminal's S3, or a small host harness until it exists) |
| N7 | **`ScanScheduler`** and the `LibraryWrite` blocker | 1-1.5 | Like `test_idle_policy` |
| N8 | **`SectorCache`.** Optional: a host FatFs model (vendored FatFs on a RAM disk) counting sector reads per walk and per open on a 20k tree of the user's shape (part 7, U14: vendoring is a download; the user said yes) | 1 (+1) | LRU, bypass, write invalidation, a random model check; the model checks metascan's 56 sectors per open before L0 |
| N9 | **The catalog, the UI and the texts**: `TrackCatalog`, `LibraryPage` rows, `UiText`, `SleepTimer`'s `kLoose`, the console's `g*` commands | 1.5-2 | `test_ui_library`, `test_sleep_timer` |
| N10 | **Firmware glue, built and not flashed**: the FatFs lister, the diskio wrapper, the card worker, streamed JPEG input, transfer thumbnails, `device.txt` | 2-3 | `pio run -e core2` and `cache_guard` |
| N11 | **A synthetic big card** (`tools/`): about 20k tiny tagged MP3, FLAC and Opus stubs in the user's shape, with no real names, for L0 without the real library (the user writes it to a card) | 0.5-1 | Its own tag dump through N6 |
| N12 | **`LibraryUpdate`**, the boot decision and the update step as a portable state machine (3.2.2, 3.4.2): the decision table with `library.tmp` recovery and the build-at-boot marker, the safe point, the memory check and deferral, the build as a card-worker job with the scan and compaction paused until the save, the fence on the loop's readers, Thumbs' pools and the queue released and restored, the restart from D alone; and its glue in `Library.cpp`, `main.cpp` and the UI's "Updating library" state, built and not flashed | 1.5-2.5 | Like `test_idle_policy`: every row of 3.2.2, a deferral then a boot that builds, a track end near the safe point, a compaction request during a build, a bad T found at the end of a build |
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
| L0 | **The M0 bench** on a full FAT32 card (the real library, or N11's) | 0.5-1 | ms per sector; open time at entry 1, 350 and 700 of a 705-entry folder; the stock walk against the 89-148 s model; PSRAM free with the UI up |
| L1 | **The sector cache on** | 1-1.5 | A write soak with the cache on (resume saves, thumbnails, the queue, `library.idx`); the SD write bench unchanged; a remount |
| L2 | **The boot and the validation walk** | 0.5-1 | Browsable in under 3.5 s at 20k; the walk about 10 s; 0 underruns over MP3, FLAC and Bluetooth during the walk |
| L3 | **The scanner** | 1-1.5 | Per-file and full-scan times idle and playing, and on the Dance page; 0 underruns; a reboot mid-scan resumes; the stack's high-water mark; the lowest internal RAM in Bluetooth mode; the loop's `pass_max` during a scan; the battery percent |
| L4 | **The update step at 20k** | 0.5-1 | The PSRAM peak and the largest block; the pause (against 3.4.2's 11-14 s) and the save; the card worker's stack high-water mark during a build; the loop stays live; a deferral and the boot that builds; the queue, the resume point and a gapless advance survive |
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
  optional model).
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
2. **PSRAM at 20k** leaves about 0.26 to 0.71 MB (a full queue of 5,000,
   its undo snapshot and AutoDJ, while a card-worker job runs) since the
   queue's cap; it was about −0.1 to 0.35 with a whole-library queue. The
   levers are in 3.5; fragmentation can defer a build to the next boot,
   which the build-at-boot marker makes happen.
3. **Sector-cache invalidation bugs would corrupt data.** L1's write soak
   comes before anything else ships.
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
