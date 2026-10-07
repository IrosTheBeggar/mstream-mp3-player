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

**Status: a design, nothing built (2026-10-07, at cc13597 on
`feature/metadata`, from dev with v0.7.0 released).** Part 2, the card
contract, is a **PROPOSAL (v1)** for the transfer software, whose own
design is still being worked on in mstream-terminal; it is written so
that side can implement it without reading the player's code, and every
place where that side is undecided says so. Part 3 is the player's side,
designed against the code at cc13597; part 5's tag rules bind both
sides. Part 6 answers the
user's question, "can we start without the Core2?": **yes.** About 19-26
agent-days of the player's work and all of the terminal's (about 9.5-12.5)
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
fixtures are what makes "agree" testable without a device.

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
│   ├─ pending.bin              MSPD: an unfinished run's plan (only while one is)
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
- Two exceptions: MD5 digests are stored as their 16 bytes in digest
  order (the order of their hex form), and thumbnail pixels are
  big-endian RGB565, as the device stores them today.
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
    stores it** (section 2.8.5).
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

The last two rows are why the software records names as the card returns
them: the same text in two normalisation forms has two hashes.

#### 2.3.4 FAT timestamp, and the uniform-skew rule

- **The value:** `fatTime` is a u32, `(fdate << 16) | ftime`: the raw FAT
  directory fields as FatFs's `FILINFO` reports them.
  - fdate: bits 15-9 the year minus 1980, bits 8-5 the month, bits 4-0
    the day.
  - ftime: bits 15-11 the hour, bits 10-5 the minute, bits 4-0 the
    seconds divided by 2.
- **0** means unknown.
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
   sizes match.
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
  - one list: at most 16 values and 1,023 bytes; values past either limit
    are dropped whole, and TRUNCATED set;
  - readers MUST accept values up to these limits, and MAY cut them
    further for display.
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

Layout rules: sections are in increasing offset order and don't overlap;
the gaps between them are zero padding of under 8 bytes; the last
section ends exactly at fileBytes; a type appears at most once per file.

#### 2.4.3 Validation

A reader MUST treat a file as **absent** when any of these fails: the
magic; a supported major; headerCrc; fileBytes equal to the length on the
card; the directory's offsets inside the file; the CRC of each section
the reader uses; the bounds checks of the section rules below.

An absent file is never an error the listener must clear: each side falls
back as section 2.9 says. The device MAY skip a large section's CRC on
later boots once it has checked that same file (by headerCrc and
generation) and noted it in `/.player`.

#### 2.4.4 String sections (`STRS`, `OSTR`)

- A blob of NUL-terminated UTF-8 strings.
- Byte 0 is NUL, so **offset 0 is the empty string** and means "absent".
- Every referenced offset MUST be inside the section, with a NUL after it
  inside the section.
- Strings are **not shared**: each is written where it is first used, in
  the order of 2.6.8 (a shared copy would save about 0.4 MB at 20k
  tracks; byte-identical output from two writers is worth more).

#### 2.4.5 Versions and compatibility

- **Major.** A reader supports a set of majors (v1 readers: {1}); a file
  of another major is absent to it. The device then behaves as if the file
  weren't there. The software MUST NOT write to a card whose
  `manifest.bin` has a major it doesn't know: it reports the card
  read-only and asks for an update.
- **Minor.** Readers accept any minor.
  - New fields go only at the **end** of a record (a larger stride) or of
    the header (a larger headerBytes).
  - A reader reads the prefix it knows and skips the rest; reading a
    shorter record than it knows, it treats the missing tail as zeros.
  - So **every field added in a minor version MUST use all-zero bytes to
    mean "absent or unknown"**.
- **New sections** are skipped by readers that don't know them, unless
  flagged REQUIRED.
- **New enum values and flag bits** read as "unknown" to older readers,
  the same as 0. Writers set only bits they define.
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

#### 2.5.1 Header (type-specific part; headerBytes = 88)

| Off | Size | Field | Meaning |
|---|---|---|---|
| 40 | 4 | commitTime | Unix seconds (UTC) by the writer's clock; informational |
| 44 | 4 | flags | bit 0 FINAL: this commit ended a run (clear: a checkpoint inside one) |
| 48 | 8 | commitId | a random u64, new for every commit |
| 56 | 16 | serverInstance | the mStream server's instance UUID as 16 bytes (zeros: unknown; part 4, A6) |
| 72 | 4 | producer | STRS: the writer and its version, `mstream-terminal 0.13.0` |
| 76 | 4 | serverRevision | STRS: the sync manifest's `revision` (its ETag) when the selection was read; "" unknown |
| 80 | 4 | baseGeneration | the generation this commit replaced (0 for the first) |
| 84 | 4 | reserved | 0 |

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
| `LIBR` | | 4 | library roots: STRS offsets of folders relative to `/music` (2.8.6); absent or empty: one root, `/music` |
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
  one MPDJ.
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
| 56 | 4 | albumValues | distinct album strings, for pre-sizing (0 unknown) |
| 60 | 4 | artistValues | distinct artist and album-artist strings, for pre-sizing (0 unknown) |
| 64 | 4 | producer | STRS: the writer and its version |
| 68 | 4 | reserved | 0 |

#### 2.6.2 Sections

| Type | Flags | Stride | Content |
|---|---|---|---|
| `FOLD` | REQUIRED | 16 | the folder table |
| `RECS` | REQUIRED | 72 | one record per file |
| `STRS` | REQUIRED | blob | names and tag strings |
| `HIDX` | | 12 | the path-hash index |
| `ORIG` | | 80 | the ledger, one row per record (sources 2 and 3; the device ignores it) |
| `OSTR` | | blob | the ledger's strings (server paths), apart so the device can skip them |

#### 2.6.3 `FOLD`: folders (16 bytes)

| Off | Size | Field | Meaning |
|---|---|---|---|
| 0 | 4 | parent | folder index; 0xFFFFFFFF for folder 0 |
| 4 | 4 | name | STRS: the folder's name as the card stores it (folder 0: offset 0) |
| 8 | 4 | flags | bit 0 OWNED: the producer created this folder (sources 2 and 3); bit 1 THUMB: `/.mstream/thumbs` has this folder's thumbnail (2.14.1) |
| 12 | 4 | firstRecord | its first record's index; its records run to the next folder's firstRecord (or recordCount) |

**Folder 0 is `/music`.** The table holds every ancestor of a record, and
every OWNED folder, even an empty one, so the software can remove it
later. A folder's path is `/music` followed by `/` and each folder's name
from the root down.

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
| 60 | 4 | picOffset | file offset of the elected embedded picture's first stored byte (0 none) |
| 64 | 4 | picLength | its stored bytes |
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
| 4 | RG_FROM_R128: an Opus R128 gain, converted: `rg = round((q7.8 / 256 + 5) × 100)` (the reference moves from −23 LUFS to −18 LUFS) |
| 5 | NO_TAGS: the file was read and carries no tags |
| 6 | UNREADABLE: the producer couldn't parse it; every tag field is absent |
| 7 | TRUNCATED: a value or a list was cut (2.3.6) |
| 8 | BPM_ANALYSED: bpm10 and camelot are mStream's analysis, not the file's tags |
| 9 | FROM_API: the tag fields are mStream's API values, not a reading of the card file (2.7) |
| 10-15 | reserved |

**`known` (u32)**, one bit per field the producer actually looked for. A
known field that is zero or empty is *absent from the file*; an unknown
one is *not reported*. The builder treats both the same way (2.9); the
bits are for diagnostics, for the parity test, and for Stage B.

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
| 70 | 1 | originFlags | bit 0 HASH_SAMPLED: the server's hashes are sampled digests (the source is above its 25 MB threshold); bit 1 VERIFIED: the download's MD5 matched `hash`; bit 2 ADOPTED: the file was on the card before and was taken into the ledger |
| 71 | 1 | convertedTo | 0: the card file is the server's bytes; else the container code (2.6.4) of a conversion |
| 72 | 2 | convertKbps | the conversion's bitrate in kbit/s (0) |
| 74 | 6 | reserved | 0 |

mStream's canonical key is `audio-hash`, else `hash`
(`COALESCE(audio_hash, file_hash)`): the key of AutoDJ rows, stars and
play counts.

#### 2.6.7 `HIDX`: the path-hash index (12 bytes)

`pathHash` u64, then `record` u32, sorted by (pathHash, record). Writers
SHOULD include it; readers MAY build their own. A hit is a candidate only:
the reader MUST compare the record's full path before using it.

#### 2.6.8 Order (deterministic output)

Writers MUST order:

- **FOLD** in pre-order from folder 0, siblings by their names' bytes
  (`memcmp`, shorter first on a common prefix);
- **RECS** by folder index, then by name bytes;
- **HIDX** by (hash, record);
- **STRS**: the leading NUL, the folder names in folder order, then for
  each record in record order its name, then its run;
- **OSTR**: the leading NUL, then each row's serverPath in row order.

So the canonical order of two files compares their folders' pre-order
positions first (an ancestor before its descendants, siblings by name
bytes), then their names. It is the order of a walk that, in each folder,
lists its files by name and then descends into its subfolders by name.
The same files and values give the same bytes from both producers, and
the conformance tests compare whole files (2.17). Note that this is not
the byte order of the whole path strings: `A` and its subfolders come
before `A B`.

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
| container | `format`, or the conversion's | `mp3` 1, `flac` 2, `opus` 3 |
| the ledger (ORIG) | the manifest entry | 2.6.6; hex digests → 16 bytes |
| size, fatTime, qfp | the card, read back | 2.3.4, 2.3.5 |
| FOLD THUMB, the thumbnail | the folder's image, else `album-art` | 2.14 |

**The fallback (FROM_API).** A software with no reader yet, or a file its
reader can't parse, MAY fill the tag fields from the API instead, and MUST
then set FROM_API and only the `known` bits of the fields it filled:

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
  (`LibraryIndex::addFile`).
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
3. **The extension** stays as the server has it; a converted file takes
   its new one (`.opus`, `.mp3`).
4. **Depth.** Past 8 folder levels, join the 8th and deeper folders into
   one, with ` - ` between them.
5. **Length.** While `/music/` + the path exceeds 255 bytes, or a name
   exceeds 255 UTF-16 units:
   1. take the longest component (ties: the deepest; for the file, its
      stem);
   2. cut it at a code-point boundary;
   3. append `~` and 4 uppercase hex digits: the low 16 bits of the
      FNV-1a 64 of the original component's bytes.
6. **Collisions.** Within a folder, compare names case-insensitively (the
   Unicode simple upper case of the NFC form, the way FAT matches). An
   existing folder's on-card spelling wins, so the folders merge. Of two
   files that collide, the one with the lower mStream id keeps the name;
   the others take ` (2)`, ` (3)`… before the extension. An "already
   exists" from the OS is also a collision.
7. **Read back** the names as the card lists them after writing, and
   record those (2.8.5).

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

The card is the reference. FatFs (UTF-8 API, long names, code page 850 on
the device) returns long names exactly as written, and a PC does too. So
the software records each name as its own listing of the card returns it,
and every path hash and every name in a record is that spelling. Names
that exist only as 8.3 short names with bytes above 0x7F may read
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
   it is paid once per file). Source: transfer.
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
  listing as W: the software listed the card moments ago. The device's
  background walk then confirms, and a mismatch makes the file's record
  fall to rule 2 or 3 at the next build (section 3.2).
- Records for paths the walk didn't find are ignored. The device MAY drop
  them from D at its next rewrite; it never touches T.
- The device's scan never reads a file rule 1 covers: transfer records
  save the scan.
- **Rescan tags** on the device rewrites D's records only; rule 1 still
  prefers a matching T record (part 7, U8).
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

1. **The server.** The manifest's `revision` (its ETag, sent back as
   If-None-Match on the first page) answers "nothing changed" with a 304.
   Otherwise a ledger row is unchanged when audioHash, serverSize and
   serverModified are, and fileHash too when present. `revision` counts
   the tracks with art, so an art backfill alone gives a new revision:
   then only the thumbnails are redone. While the manifest says
   `scanning: true`, a missing path MUST NOT be read as a deletion.
2. **The card.** List `/music` and compare each ledger file's (size,
   fatTime) with the listing, applying the skew rule (2.3.4) to the
   previous ledger.
   - Equal: unchanged.
   - Size equal, time different, no skew: compare qfp. Equal: unchanged
     (record the new fatTime).
   - Otherwise: changed by the listener.
   - Missing: deleted by the listener.
3. **The plan:**

| Server | Card | Action |
|---|---|---|
| new in the selection | path free | write |
| changed | unchanged | replace |
| unchanged | unchanged | nothing (re-list its stamp) |
| any | changed by the listener | keep the listener's file, drop it from the ledger, report it (part 7, U5) |
| in the selection | missing | write again (part 7, U4) |
| left the selection | unchanged | delete |
| left the selection | changed by the listener | keep, drop from the ledger, report |

An optional **deep check** compares qfp for every owned file: two reads
each, about 1-2 minutes for 20,000 files through a USB reader
(ESTIMATED).

### 2.11 Who may write and delete what

| Path | The software | The device | The listener |
|---|---|---|---|
| `/music/**` files in the committed ledger whose (size, fatTime) still match it | create, replace, delete | read | anything |
| `/music/**` all other files | read and list only (qfp allowed) | read | anything |
| `/music/**` folders | create; remove only OWNED folders that are empty | none | anything |
| `/music` itself | create if missing | read | anything |
| `/.mstream/**` | everything | **read only** (except as the sync agent: 2.12.4) | may delete it all (the device then scans; the software then sees a card with no ledger) |
| `/.player/**` | **read only**; it relies on `device.txt` alone | everything | may delete it all (the device rebuilds) |
| anything else on the card | never | never | anything |

- **A file the listener changed** stops being the software's at the next
  run (2.10.2). The software MUST NOT replace or delete it without the
  listener's explicit say-so.
- **A card whose root names another server** (both serverInstance values
  known, and different): the software MUST NOT delete or replace the files
  that root owns without asking (part 7, U7).
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

1. **Settle the root.** Pick it (2.5.4). If `manifest.tmp` won, finish its
   rename: delete `manifest.bin`, then rename.
2. **Clean up.** Delete everything in `stage/`, every `*.tmp` in
   `/.mstream` and its `thumbs/` folders, and every companion the root
   doesn't name. If `pending.bin` exists, the last run was cut short: its
   write targets whose identity doesn't match the ledger are re-copied
   (2.12.3).
3. **List** the card; read the server's selection; plan (2.10.2).
4. **Write `pending.bin`** (MSPD, 2.12.5) through `pending.tmp` and a
   rename: the paths this run will write or delete.
5. **Delete first.** Ledger files leaving the card go, only when their
   identity still matches. This frees space on a full card; a cut here
   only leaves fewer files.
6. **Copy each file:**
   1. download into `stage/<n>.tmp` (an 8.3 name; n is the op's number in
      hex), computing qfp, and the MD5 when the server's hash is a full
      digest;
   2. check the MD5 against `hash` when HASH_SAMPLED is clear and the file
      isn't converted (VERIFIED);
   3. close it and rename it into its `/music` path, creating folders
      (OWNED) as needed;
   4. read back its name, size and fatTime; read its tags (2.7).
7. **Checkpoint** every 500 files or 5 minutes:
   1. write `tags-<g>.bin` (g = the root's generation + 1) under its final
      name;
   2. write `manifest.tmp` (FINAL clear), delete `manifest.bin`, rename;
   3. delete the previous tags file;
   4. rewrite `pending.bin` with the rest of the plan;
   5. go on in the next generation.
8. **Thumbnails:** for each album whose art is new or changed, write
   `thumbs/<h>/<8 HEX>.tmp`, then rename it to `.565`.
9. **AutoDJ:** write `autodj-<g>.bin` when 2.13.4 says so; otherwise the
   new root names the old one again.
10. **The final commit** is step 7's write and rename with FINAL set; then
    delete `pending.bin`.
11. **Collect:** delete the companions the root doesn't name, thumbnails
    whose folder no longer has THUMB, and empty OWNED folders.

**Renames over an existing file:** Rust's `fs::rename` may or may not
replace atomically on a FAT driver (Windows `MoveFileExW` with
REPLACE_EXISTING is not atomic on FAT). The root rule (2.5.4) makes either
outcome safe. Other files are named so that the old and the new never
share a name, except a replaced music file: a single rename over the old
one, and the identity rules catch both outcomes.

#### 2.12.2 A card pulled during a run

| Pulled during | What the card holds | The device | The software's next run |
|---|---|---|---|
| a download | a partial `stage/n.tmp` | never sees it (a hidden folder) | deletes it (step 2) |
| the rename into `/music` | the old file or the new one | identity mismatch: scans the new one itself | re-copies it (pending, 2.12.3) |
| the deletions | some files gone | gone files are absent | plans again |
| a tags file's write | a partial `tags-<g>.bin` nothing names | ignored | deletes it |
| `manifest.tmp`'s write | an invalid tmp | uses `manifest.bin` | rewrites it |
| between deleting `manifest.bin` and the rename | a valid tmp only | uses the tmp | finishes the rename |
| a thumbnail | a partial `.tmp` | ignored (wrong name; the header is checked too) | deletes it |
| the collection | leftovers | ignored | collects again |

**FAT itself:** an interrupted directory or FAT update can leave lost
clusters, and rarely a cross-linked chain. The software SHOULD ask for
the card to be ejected before it is pulled, and MAY offer the OS's disk
check after a cut.

#### 2.12.3 Recovering a cut-short run

`pending.bin` lists each write target with its expected size. For each
one: if its identity equals the ledger's (the rename never happened, or
the commit did), nothing to do; otherwise it is the software's
half-finished work: re-copy it. A listener's own file placed at exactly
that path between the two runs is the one case this gets wrong, accepted
as too rare to matter.

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
  write, 2 delete), flags (u8, 0), 2 reserved bytes, path (u32, STRS,
  relative to `/music`), expectedSize (u32), reserved (u32); and `STRS`.
- **The device** reads only the file's presence ("the last transfer
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
| 51 | 1 | scoreKind | 1: u8 = clamp(round(cosine × 255), 0, 255) |
| 52 | 4 | builtTime | Unix seconds; informational |
| 56 | 16 | selectionSig | 2.13.3 |
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
| 0 | 8 | pathHash: the smallest path hash among the row's card files (its primary file) |
| 8 | 8 | hashPrefix: the first 8 bytes of the canonical hash (`audio-hash`, else `hash`) |
| 16 | 2 | bpm10, as in RECS (the file's tag, else the server's analysis) |
| 18 | 1 | camelot, as in RECS |
| 19 | 1 | flags: bit 0 BPM_ANALYSED |
| 20 | 4 | artistKey: the low 32 bits of the FNV-1a 64 of nameKey (5.4) of the primary file's first artist value; 0 none |

**`DJNB`:** row r's list starts at r × stride.

- Each entry is a row index (indexBytes, little-endian), then a score byte.
- Entries are sorted by score descending, then index ascending.
- Unused slots have an index of all ones and a score of 0.
- A list never holds its own row, nor a row of the same song: equal
  `nameKey(artist display) + "|" + nameKey(title)` (5.4) of the rows'
  primary files.

**`DJPH`:** `pathHash` u64, then `row` u32, sorted by (pathHash, row).
Every card file that has a row appears; duplicates (one recording at
several paths) share one row.

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
  filters**: BPM, key and genre from the chosen record when present,
  otherwise no filter.
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
- **Key:** the album **folder's** path hash (2.3.3), for example of
  `/music/Artist/Album` (the device's own thumbnails key on the cover
  file's path; a transfer thumbnail needs no cover file on the card).
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
  thumbnail it writes, and removes thumbnails whose folder lost the flag
  (2.12.1, step 11).

#### 2.14.2 The source image: folder first, as the device would choose

So that a transfer thumbnail shows what the device itself would have
shown, the software elects the source by the user's folder-first rule:

1. **The server folder's image**, ranked as the device ranks them:
   `cover`, then `folder`, then `front` (`.jpg` or `.jpeg`, any case),
   then the largest other `.jpg`. PNG and progressive JPEG are fine here:
   the PC decodes them. Today the folder can't be listed through the API,
   so the software probes the named files with `HEAD /media/<vpath>/<dir>/<name>`;
   a listing (part 4, A4) makes the ranking exact.
2. **Else** the most common `album-art` among the folder's tracks,
   fetched with `GET /album-art/<file>` (the full image, not the `zl-`
   and `zs-` copies). That is mStream's choice: embedded art first by
   default (`albumArtPriority: 'metadata'`), or an online lookup.
3. **Else** no thumbnail.

#### 2.14.3 The device's cover order for an album

1. **The transfer thumbnail**, when the album's folder has THUMB in a
   valid T, the file reads back as MPTH v1 with the folder's hash, and the
   folder holds **no cover image the ledger doesn't list** (a `.jpg` the
   listener added by hand wins, by step 2).
2. **The folder image**, through the device's own thumbnail cache
   (`/.player/thumbs`, keyed by the image's path), else decoded: `cover`,
   `folder`, `front`, then the largest other `.jpg` (`LibraryIndex::imageRank`;
   `Thumbs.cpp`'s `pickLargest()` when more than one other `.jpg`). Today's
   behaviour.
3. **The embedded JPEG** (milestone L6): the picture of the first track, in
   album order, whose chosen record has `picOffset` and `picMime` = JPEG;
   read from `picOffset` for `picLength` bytes, unsynchronised when
   `picCoding` is 1, decoded by the existing baseline decoder.
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
max_rate=48000
max_channels=2
```

- `read.*` lists the majors the firmware reads, separated by commas.
- **The software** writes the newest major of each format that the device
  lists, and converts (or skips) anything not in `codecs`, or above
  `max_rate` or `max_channels`.
- **No file** (a new card, or firmware before this design) means
  `read.*=1` and `codecs=mp3,flac`. Firmware 0.7.0 plays Opus but can't
  say so; the software MAY ask the listener rather than convert.

### 2.16 Compatibility summary

| Situation | Outcome |
|---|---|
| The device meets a newer major | That file is absent: no transfer data, the device scans the card itself. Slower, never wrong. `device.txt` lets the software avoid it |
| The device meets a newer minor | It reads the prefix it knows; new sections and fields are skipped |
| The software meets a root of a newer major | Refuses to write; says it needs an update |
| The software meets an older major | Reads it if it still can, and rewrites everything in the newest major the device reads |
| An unknown REQUIRED section | The file is absent |
| An unknown COMP kind | Ignored |
| An unknown flag bit or enum value | Treated as unknown or absent |
| Firmware with no `device.txt` | The software writes v1 of everything |
| Damage (any CRC) | That file is absent; a damaged root means no transfer data |
| A new string field | Appended to the run; older readers stop at the fields they know |

### 2.17 Conformance (no device needed)

Both implementations test against shared fixtures, kept in the player
repo (proposed: `test/fixtures/card/`, under a permissive licence so
mstream-terminal and mStream can copy them; part 7, U18) and mirrored
into mstream-terminal:

1. **The vectors** of 2.3.2-2.3.5 and 2.13.3, and the FAT example in
   2.3.4.
2. **Writer equality.** A JSON description of a small library (folders,
   files, sizes, stamps, tag values, ledger fields) goes into both
   writers. The C++ writer (host-built, `lib/core`) and the Rust writer
   MUST produce byte-identical MPTG files (2.6.8), and the same for MSMF,
   MPDJ, MSPD and MPTH.
3. **Reader parity.** A corpus of synthetic audio files (no real
   library's files) goes through the software's reference reader and the
   device's TagScan built on the host. Their records MUST be field-equal,
   with durationMs within ±100 ms (lofty applies no MP3 encoder-delay
   trim, the device does: about 25-50 ms) and FROM_API and BPM_ANALYSED
   fields excluded. The corpus includes multi-value frames, APE plus
   ID3v1 without ID3v2, ID3v1 filling a blank ID3v2 field, bad UTF-8,
   odd-length UTF-16, ISO-8859-1 bytes 0x80-0x9F, v2.4 tag-level
   unsynchronisation, non-syncsafe v2.4 sizes, a FLAC with a front ID3v2,
   Opus R128 gains, and pictures behind large frames.
4. **Reader hardening:** truncation at every byte, a flipped bit in every
   section, offsets out of range, strings with no NUL, counts that
   disagree: all make the file absent, with no crash. Fuzzed on the host.
5. **The builder** (host test): a walk listing plus T plus D gives the
   expected `library.idx` checksum; a card filled by the software and the
   same files scanned by the device give the same index (the research's
   M7 test, on the host); the skew rule: every stamp shifted by +3,600 s
   still matches, three shifted files don't make a skew.
6. **Crash safety** (host, a fake file system that can stop at any
   write): every cut point of 2.12.1 leaves a card the device reads as the
   old or the new commit, and the next run brings it to the planned state.

### 2.18 Sizes at 20,000 tracks (ESTIMATED)

| File | Size |
|---|---|
| `manifest.bin` | under 4 KB |
| `tags-<g>.bin` | about 6.9 MB: RECS 1.44 MB, STRS about 2.4 MB (names about 31 B and runs about 87 B per track), HIDX 0.24 MB, ORIG 1.6 MB, OSTR about 1.2 MB. The device reads FOLD, RECS and STRS: about 3.9 MB, 2.3-3.3 s at 1.2-1.7 MB/s, during a build only |
| `autodj-<g>.bin`, K = 100 | about 6.7 MB (DJNB 6.0 MB); one 300-byte row read per pick |
| `/.mstream/thumbs` | about 39 MB for 1,800 albums |
| `/.player/tags.bin` | about 2.1 MB when the software covers every file (status rows, names); about 3.9 MB when the device scanned everything |

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
| v6, hard inputs match | any | **Load it:** 1.8 MB, 1.1-1.5 s at 20k (ESTIMATED). No walk. | The validation walk (3.2.3). If the soft inputs differ (the scan went on after the last build), resume the scan; rebuild at its end. |
| v6, the transfer's identity differs (a transfer happened, or `/.mstream` is gone or damaged) | T valid, or D only | Compact the journals if any (0-3 s). **Build from T (if valid) and D** behind the boot screen, a new T's listing standing in for the walk (2.9): about 4-6 s at 20k (ESTIMATED), then the save. Without T, D's rows for files T covered turn Pending. | The walk confirms; files it finds changed go Pending; the scan reads the Pending files. |
| an older rules version, v1-v5 (today's is v5), missing or corrupt | some | Build from the records. No walk. | The walk. |
| v1-v5, missing or corrupt | none: a card-reader card, or this firmware's first boot on a 0.7 card | **Walk now**, with a progress line, into a path-named index and a D of all-Pending entries: about 9-11 s at 20k with the sector cache (89-148 s without); about 70 ms on today's card. | The scan (3.3). |
| any | NoMemory | As today: no library; the built-in tracks still play. | none |

- The queue is then restored as today (`QueueStore::restore()`).
- **A power cut mid-save:** if `library.idx` is missing and `library.tmp`
  loads with a good checksum, it is used and renamed (power went between
  the remove and the rename). `queue.tmp` already follows this rule.
- `LibraryIndex::Load::Stale` no longer happens at boot: the path
  signature isn't computed any more.
- **`device.txt`** (2.15) is rewritten here when its content would change
  (a new firmware): one small write.
- **`pending.bin`** present: the Library tab's status line says the last
  transfer didn't finish.

#### 3.2.3 The validation walk (the card worker, every boot, about 2 s after the UI's first frame)

**The lister.** FatFs `f_opendir`/`f_readdir` on `"0:/music/..."` (the SD
driver mounts drive `"0:"`). `FILINFO` gives `fsize`, `fdate` and `ftime`
in the same directory read, so there is no `stat()`. The LittleFS
fallback keeps today's POSIX walk, with `st_mtime` packed into a FAT time.

**One folder at a time:**

1. Read all its entries, then close it: one DIR open at a time.
2. The long-name buffer is on the caller's stack
   (`CONFIG_FATFS_LFN_STACK`, 255), so the worker's stack stays flat.
3. Sort the entries into the canonical order (2.6.8) in a PSRAM scratch of
   at most 64 KB (`/music` with 705 children is about 28 KB; a bigger
   folder goes in FAT order and is logged).
4. Its digest: FNV-1a 64 over (name, size, fatTime) of its audio and image
   files, and its count of other files.

**Comparing with what the device knows:**

- D's device-private folder table is read once (about 0.1 MB at 20k).
- **Equal digest:** nothing to do for that folder. This is the normal
  boot.
- **Different digest:** merge its files against D's records for that
  folder (a seek to the folder's range).
- **The first walk after a commit** (D's walk identity differs from the
  root's): merge against T's records too, one near-sequential pass over
  T's FOLD, RECS and STRS (about 3.9 MB, 2.3-3.3 s at 20k).

**T's records, per file** (2.9 rule 1): size and time equal is a match;
size equal and time different is *doubtful*, with its Δ kept in a small
histogram. At the walk's end the skew (2.3.4) is computed from it;
doubtful files whose Δ is the skew match; the rest get a qfp check (an
open and two 4 KB reads, about 8 ms with the cache, ESTIMATED). A match
is saved in D as a confirmation, so it is paid once per file, not per
boot. Worst case (a PC that converted each stamp differently): about
2.7 min of checks once at 20k.

**Output:** `walk.jnl`, one sorted run of Added (path, size, time, Pending
or Software), Changed, Gone, FolderCover (the best image's name, rank,
count, size, time, and whether T's ledger lists it) and Confirmed (the
commit). Nothing is written when nothing changed.

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
| Restore the queue (`queue.txt`) | ms | 0.1 s (a short queue) to 1.5-2 s (20k lines, about 1.5 MB, `findTrack` per line) |
| **To a browsable library** | **under 0.5 s** | **about 1.5-3.5 s** |
| Background walk, cached | under 0.05 s | 9-11 s, plus 2.3-3.3 s on the first walk after a transfer |
| qfp checks (only after a PC wrote times the skew rule can't explain) | none | at most about 2.7 min once |

If L2 measures more than 3 s for the queue, a binary fast path (track ids
saved with the index's build stamp, used when the stamp matches) can come
later; `queue.txt` stays the fallback.

### 3.3 The scanner

#### 3.3.1 Which files it reads

An audio file is scanned only when all three hold:

1. no T record matches it (2.9 rule 1, after the walk's confirmations);
2. no D record of the same size and time, the current parser version and
   the current rescan epoch;
3. it isn't marked Unreadable at the same size and time (a failed parse
   isn't retried until the file changes or the listener asks).

Images and other files are never parsed; folder cover facts come from the
walk.

**Per file** (metascan section 5.1-5.2): an open, 1-3 reads of 4 KB, a
close: 10-60 ms. `TagScan` reads through a FatFs `Source` (`f_lseek` +
`f_read`); its 4 KB buffer and its record (about 3 KB at the limits of
2.3.6) are in PSRAM. Durations come from the existing helpers
(`progress::mp3HeaderDurationMs` with the LAME trim, `flacDurationMs`;
Opus from the last granule). Pictures are located, never read. The
reading rules are part 5's.

#### 3.3.2 `tags.bin`, the journals, compaction, resume

| Item | Rule |
|---|---|
| **`/.player/tags.bin`** | MPTG source 1 (2.6), in canonical order, one record per audio file the last walk saw, plus device-private sections (their layout is the device's own): `DSTA`, a status per record (**Software**: a T record confirmed at the current commit, the row carrying size and time only; **Scanned**: a full record; **Pending**; **Unreadable**), with a bit for "confirmed by qfp"; `DFLD`, per folder: the digest and the cover facts; `DHDR`: the commit the last walk compared against, T's skew, the rescan epoch. Size at 20k: about 2.1 MB if every file is Software, about 3.9 MB if every file is Scanned. |
| **`tags.jnl`** | Chunks: a magic, a sequence, the headerCrc of the `tags.bin` it extends, a count, the records, a CRC-32. One chunk every 100 files or 5 s: 15-20 ms per append (MEASURED for small writes). A torn last chunk fails its CRC and is dropped; a chunk for another `tags.bin` was already merged and is dropped. |
| **`walk.jnl`** | The last walk's changes, one sorted run (3.2.3). |
| **Compaction** | When `tags.jnl` reaches 512 KB (about 3,500 records), at a scan's end, and before any build: a 3-way merge into `tags.tmp` (`tags.bin` streamed, `walk.jnl` streamed, `tags.jnl` read whole into PSRAM and sorted). A full 20k scan compacts about 6 times: about 12-24 s of writes at 0.5-1 MB/s, in the background (ESTIMATED). |
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
end (part 7, U11).

#### 3.3.4 Yielding (a portable `ScanScheduler`, host-tested like `test_idle_policy`)

**One card worker, shared with the thumbnails.** It generalises Thumbs'
worker: core 1; priority 1 while nothing moves, 0 while a list moves,
always below the decoder's 2; a 6 KB internal stack that exists only while
there is work, gone 3 s after the last step. A step is one file, or one
folder of the walk.

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
Bluetooth mode has the least internal RAM. L3 measures the lowest
internal free during a scan in Bluetooth mode.

#### 3.3.5 Battery and power (the user's choice: on battery, while playing)

- **Cost:** about 1% of a 390 mAh charge for a full 20k scan with the
  cache (ESTIMATED, metascan section 6.4).
- **Low battery:** pause below 10% when not charging; resume on USB or
  above 15% (part 7, U13).
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
- **The listing** (which files exist) is D's entries. When D's walk
  identity is older than the root's (no walk since the commit), T's paths
  count as present too: section 2.9's "before the first walk". A T path a
  walk at this commit didn't see is left out.
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
  boot", and the build runs at the next boot, before the UI, on a fresh
  heap.

**The sequence:**

1. `queueStore.flushNow()`: `queue.txt` now *is* the queue.
2. The UI enters "Updating library": Now Playing keeps a copy of the
   playing track's title, artist and album; the lists show the status
   line; queue edits and seeks wait; the decoder keeps its open file.
3. Free Thumbs' pools (about 315 KB; `libraryChanged()` clears them
   anyway), AutoDJ's maps, the queue's entries and undo snapshot (a new
   `QueueModel::release()`), and **the old index** (`LibraryIndex::clear()`).
4. Build: `begin(Sizing)` from the headers' counts (exact: no doubling,
   no slack), the merge feeding `addRecord()` and `addFile()`, then
   `finish()` (sort, votes, views, trim).
5. Live again: re-read `queue.txt` with `restore()`'s logic, carrying the
   resume start point as `remap()` does today, its sinks pre-sized from
   the file's entry count; lengths from the index; AutoDJ's join (3.5);
   Thumbs' pools back; `userInterface->libraryChanged()`.
6. Save `library.idx` on the card worker, aside then renamed, under the
   `LibraryWrite` blocker. The index isn't changed while it is written.

**Time (ESTIMATED):** the pause (steps 2-5) is about 4-6 s at 20k (about
6 MB of sequential reads at 1.2-1.7 MB/s, plus about 1 s of sorting),
under 1 s at 2k; the save follows in the background (about 1.8 MB at
0.5-1 MB/s, 2-4 s).

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
- **`load()`:** Loaded (compare the inputs: a hard mismatch rebuilds from
  the records at boot, a soft one keeps the index and rebuilds at the
  scan's end); Outdated (versions 1-5, or an older `rulesVersion`: build
  from the records, else walk); Corrupt (an older firmware sees v6 this
  way: it rebuilds its own v5 from a walk and ignores `/.mstream` and
  D; this firmware then treats that v5 as Outdated: one rebuild each
  way, nothing lost); NoMemory as today.
- **Size at 20k (ESTIMATED):** tracks 640 KB, albums (about 1.8k) 50 KB,
  artists 14 KB, folders (about 2.6k) 94 KB, views about 190 KB, strings
  about 0.8 MB (today's names about 0.68 MB plus about 0.12 MB of tag
  strings): **about 1.75-1.85 MB, 88-92 B per track** (75-79 B today,
  MEASURED).

### 3.5 Budgets

**PSRAM at 20k tracks, with AutoDJ loaded** (ESTIMATED from MEASURED
counts):

| Item | MB | Basis |
|---|---|---|
| Free with the UI up, today (77-track card) | **2.77-2.90** | MEASURED (dev.log `psram=`) |
| `library.idx` v6 | −1.75 to −1.85 | 3.4.3 |
| A whole-library queue, 20k entries, with its undo snapshot | −0.48 (−0.96 untrimmed) | 12 B per entry, twice (`QueueModel`). With no saved queue the boot queues the whole library (`queueEverything`), so this is the *default* on a new card |
| AutoDJ: u16 maps, filters, 5 cached rows | −0.17 to −0.33 | autodj research section 5 |
| The sector cache | −0.07 | 3.2.4 |
| `DurationBook` | −0.04, or 0 once lengths come from the index | `lib/core/QueueView.h` |
| Scanner and walker, only while active | −0.03 to −0.08 | the buffer, the record, the folder scratch, the journal buffer |
| **Headroom** | **about 0 to 0.35** | negative in the worst case without the trims below |

**What that forces:**

- **Covers stream.** Thumbs reads a whole JPEG into PSRAM (capped at
  2 MB), which doesn't fit at 20k. TJpgDec's input is fed from the file
  in 4-16 KB chunks instead, which also lifts the 2 MB cap. The scaler's
  about 170 KB during a decode fits only in the better half of the range,
  so transfer thumbnails (one 3.2 KB or 18 KB read, no decode) matter at
  20k.
- **The queue's blocks are trimmed** to their exact size after
  `assign()`.
- **During the update step:** it frees the index, the queue, AutoDJ's
  maps and Thumbs' pools (about 2.7-3.4 MB with the headroom); the build
  peak, pre-sized, is about 2.0 MB; margin about 0.7 MB or more.
  Afterwards, in order: AutoDJ's join (a sorted array of (path hash,
  track id), 240 KB transient, then 2 × 40 KB of u16 maps; rows whose
  file is gone are skipped), the queue's re-read (about 80 KB transient,
  then 240 KB), Thumbs' pools (315 KB).
- **Fragmentation:** the arena (about 0.8 MB) and the track table (640 KB)
  are single blocks; the memory check (3.4.2) defers a build to the next
  boot rather than fail.

**Levers, if L0 or L4 measure less** (part 7, U12):

| Lever | Saving |
|---|---|
| No undo snapshot for queues over about 5,000 entries | 0.24 MB |
| File names kept on the card instead of in PSRAM (about 31 B per track) | 0.62 MB |
| `DurationBook` folded into the index | 0.04 MB |
| AutoDJ at its compact layout | up to 0.16 MB |

The ceiling stays about 25k tracks with a full queue (metascan section
6.2).

**Flash, IRAM, internal RAM** (metascan section 6.1): TagScan about 11.4
KB of code and 4.0 KB of rodata (MEASURED with the ESP32 toolchain), 0
IRAM, about 1 KB of stack per parse; the scan engine, the contract kit,
the builder and the UI about 10-20 KB (ESTIMATED); the sector-cache
wrapper 1-2 KB. The app is 2.3 MB in a 6.3 MB slot: flash is no
constraint. IRAM is untouched, and the `cache_guard` build check still
applies to any layout shift.

**Card time** at 20k (ESTIMATED): reading T's FOLD, RECS and STRS, about
3.9 MB, takes 2.3-3.3 s, only during a build; an AutoDJ pick reads one
300 B row (about 3 ms).

### 3.6 What changes, file by file

| File | Change |
|---|---|
| New `lib/core/CardContract` | The contract kit: CRC-32, FNV-1a 64, qfp, FAT time and the skew rule; MSMF, MPTG, MPDJ and MSPD readers and writers (the device writes only MPTG; the writers serve the host tests and the future sync agent); the root election; `device.txt`; the canonical order. Its golden files are 2.17's. |
| New `lib/core` modules | `TagStore` (D, `tags.jnl`, `walk.jnl`, compaction, recovery); `CardWalk` (an `IDirLister`, the canonical sort, the digests, T's freshness, the skew, the merge); `SectorCache`; `TagScan` (the production port of the prototype, with part 5's rules); `ScanScheduler`; `LibraryBuilder` (the merge into `LibraryIndex`, part 5's votes). |
| `lib/core/LibraryIndex.{h,cpp}` | v6 records and the header's inputs; `begin(const Sizing&)` with exact counts; `addRecord(path, const TagView&)` next to `addFile()`; Stage A's votes and orders in `buildViews()` (a missing number sorts last, an artist's albums newest first); `readNames()` fills only the fields a record lacks; `kLoose` and the transfer-thumbnail flag; library roots (LIBR), if the vpath layout is chosen. `Load::Stale` no longer happens at boot. |
| `lib/core/TrackCatalog.{h,cpp}` | `title()` the tag's own string or the slice; `artist()` the track artist, else the album's line, else the folder artist; `album()` the display name; `durationHintMs()` the library's length; a one-slot overlay for the playing track's fresh tags (3.3.3). |
| `src/app/Library.{h,cpp}` | The boot decision (3.2.2) replaces `begin()`'s walk; `rebuild()` becomes the update step; the save moves to the card worker; `report()` gains the scan state. |
| `src/app/QueueStore.cpp`, `lib/core/QueueModel` | `remap()` through `queue.txt` (flush, free, rebuild, re-read); the reads pre-size their sinks; `QueueModel::release()` and an exact-size trim. |
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
| A6 | A stable server instance id in ping. mStream already makes one (`discovery.mdns.instanceId`, a random UUID in its config, sent only over mDNS). | Binds a card to a server (MSMF `serverInstance`) without storing a URL or a token on the card. | Zeros: "unknown"; the server's URL in the software's private state. | 0.25 |
| A7 | Raw per-file tags in the manifest (`tag_album`, `tag_album_artist`, `tag_compilation`, the release id, raw credits). | Only if a route ever needs the file's own tags without its bytes. A transfer downloads the bytes and reads them, so likely never. | Reading the bytes (2.7). | 0.5-1 |
| A8 | A server-side table, `POST /api/v1/sync/autodj` (autodj research section 6): the selection in, one MPDJ out, built in a worker and cached. | Only if building in the terminal is unwanted. | The terminal builds it (about 3-25 s of compute at 10k, ESTIMATED). | 2-3 |
| A9 | ReplayGain album gain and peaks stored (a migration and both scanner engines). | Only if the device will apply album gain (part 7, U16); the records carry it from the file either way. | The device's and the software's own reading. | 1-2 |

A1-A6 together: about 1.5-2 days.

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
| bpm10 | Vorbis `BPM`, else ID3v2 `TBPM`: trimmed, parsed as a decimal number, rounded to an integer, kept only from 20 to 300; bpm10 = that × 10. (The server's analysed BPM, 2.7: round(bpm × 10), BPM_ANALYSED.) |
| camelot | TKEY, or Vorbis `INITIALKEY` or `KEY`: trimmed, its first 12 characters, matched case-insensitively against the aliases of mStream's `CAMELOT_TO_KEYS` (`src/api/random.js`: `8A`, `A minor`, `Am`, `Amin`, …); no match is 0. |
| ReplayGain | `REPLAYGAIN_TRACK_GAIN`, `REPLAYGAIN_ALBUM_GAIN` (TXXX, Vorbis, APE): trimmed, a trailing `dB` (any case) stripped, parsed as a decimal, × 100, rounded; the peaks × 10,000. Opus `R128_TRACK_GAIN` and `R128_ALBUM_GAIN` converted (2.6.4, RG_FROM_R128). mStream reads only the track gain; the record keeps all four. |
| compilation | TCMP or COMPILATION: `1` or `true` (any case) is 1; `0` or `false` is 2 ("said no"); anything else, or none, 0. mStream keeps only "yes". |
| picture | Every non-empty embedded picture is seen; the elected one is the first front cover (type 3), else the first picture. Its offset, stored length, type, MIME and coding (2.6.4) are recorded; its bytes are never read by the scan. |

### 5.4 What the builder shows (the election)

These are the device's Stage A rules; a terminal view of a card would use
the same.

**Text helpers** (mStream's `src/db/name-key.js`):

- **nameKey:** collapse whitespace runs to one space and trim; fold the
  Unicode quotes (`‘ ’ ‚ ‛ ′` to `'`, `“ ” „ ‟ ″` to `"`) and dashes
  (`‐ ‑ ‒ – — ― −` to `-`); lowercase. No accent folding.
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

### 6.1 NOW: the player, host-only (about 19-26 days)

All of it is `lib/core` code with Unity tests under `pio test -e native`,
or firmware glue that is built (`pio run -e core2`, with the IRAM
`cache_guard` check) but not flashed.

| # | Work | Days | Host proof |
|---|---|---|---|
| N1 | **The contract kit** (`lib/core/CardContract`): CRC-32, FNV-1a 64, qfp, FAT time and the skew rule; MSMF, MPTG, MPDJ and MSPD readers and writers; the root election; `device.txt`; the fixtures of 2.17 (the vectors, the JSON library descriptions and their golden files), frozen for the terminal's tests | 2-2.5 | Round trips; truncation at every byte and a flipped bit per section give "absent"; a newer major is absent, a newer minor reads; the golden bytes |
| N2 | **`LibraryIndex` v6 and `LibraryBuilder`**: Stage A's election (5.4), the merge (2.9), exact sizing, the inputs, LIBR roots | 3.5-4.5 | `test_library_index` extended; `LibrarySynth` with synthetic tags at the measured disagreement rates; 20k memory and build-peak asserts; the same files from T and from D build byte-identical indexes |
| N3 | **The queue's remap through `queue.txt`**; `QueueModel::release()` and the exact trim | 1-1.5 | `test_queue`: a 20k remap within budget; shuffled; the current track gone; the resume point carried |
| N4 | **`TagStore`**: D with its device sections, `tags.jnl`, `walk.jnl`, compaction, recovery | 2-2.5 | A power cut injected at every write, sync, remove and rename |
| N5 | **`CardWalk`**: the lister interface, the canonical sort, the digests, T's freshness (the skew, qfp, confirmations) | 1.5-2 | Fake FAT trees: shuffled order, a retag at the same size, a renamed folder, a deleted album, every stamp shifted an hour, three files shifted |
| N6 | **`TagScan`, the production port** with part 5's rules; the synthetic parity corpus (2.17, item 3) | 3-4 | The corpus and the crafted edge files; the fuzz harness (ASan only if a Linux toolchain is available); parity against a lofty reference (the terminal's S3, or a small host harness until it exists) |
| N7 | **`ScanScheduler`** and the `LibraryWrite` blocker | 1-1.5 | Like `test_idle_policy` |
| N8 | **`SectorCache`.** Optional: a host FatFs model (vendored FatFs on a RAM disk) counting sector reads per walk and per open on a 20k tree of the user's shape (part 7, U14: vendoring is a download) | 1 (+1) | LRU, bypass, write invalidation, a random model check; the model checks metascan's 56 sectors per open before L0 |
| N9 | **The catalog, the UI and the texts**: `TrackCatalog`, `LibraryPage` rows, `UiText`, `SleepTimer`'s `kLoose`, the console's `g*` commands | 1.5-2 | `test_ui_library`, `test_sleep_timer` |
| N10 | **Firmware glue, built and not flashed**: the FatFs lister, the diskio wrapper, the card worker, streamed JPEG input, transfer thumbnails, `device.txt` | 2-3 | `pio run -e core2` and `cache_guard` |
| N11 | **A synthetic big card** (`tools/`): about 20k tiny tagged MP3, FLAC and Opus stubs in the user's shape, with no real names, for L0 without the real library (the user writes it to a card) | 0.5-1 | Its own tag dump through N6 |
| | **Total** | **about 19-26** | |

**Order:** N1, then N2 and N3: they fix the shared format (which unblocks
the terminal's tests) and the builder both producers feed. Then N4, N5
and N6 in parallel; then N7-N10; N11 before the device session.

### 6.2 NOW: the transfer software (mstream-terminal; a proposal for its team)

All testable on a PC with a folder standing in for the card (or a FAT32
card in a reader), and an e2e leg against a canned server. If the contract
is adopted as written:

| # | Work | Days |
|---|---|---|
| S1 | Client methods for `sync/manifest`, `metadata/batch` and `local/embeddings`; card detection (removable FAT32 only; refuse exFAT, NTFS and GPT, saying why) | 1 |
| S2 | The path rules (2.8.3), the planner (2.10.2), the download, conversion and read-back pipeline, the FAT-time recipes (2.3.4) | 2.5-3 |
| S3 | The reference reader: lofty 0.25 with part 5's rules ported from the rust-parser (GPL-3.0 to GPL-3.0-only is fine) | 1-1.5 |
| S4 | The MPTG, MSMF and MSPD writers, generations, checkpoints, recovery (2.12) against the shared fixtures | 1.5-2 |
| S5 | Covers: the folder-first source (2.14.2) and MPTH thumbnails with the `image` crate it ships | 1 |
| S6 | AutoDJ: embeddings, an exact cosine top-K over the selection only (blocks across threads, no new crate), the MPDJ writer | 1-1.5 |
| S7 | The page (a `mstream-player device card` command, later a page in the GUI's MP3 Player tab) and the e2e leg | 1.5-2.5 |
| | **Total** | **about 9.5-12.5** |

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
| L3 | **The scanner** | 1-1.5 | Per-file and full-scan times idle and playing; 0 underruns; a reboot mid-scan resumes; the stack's high-water mark; the lowest internal RAM in Bluetooth mode; the battery percent |
| L4 | **The update step at 20k** | 0.5-1 | The PSRAM peak and the largest block; the pause and the save; the queue, the resume point and a gapless advance survive |
| L5 | **The UI at 20k** | 0.5-1 | Scroll smoothness; the status line's cost; streamed covers |
| | **Total** | **about 4-7** | |

Then, when the parts they need exist:

| # | Work | Days | Device checks |
|---|---|---|---|
| L6 | **Embedded JPEG covers** (2.14.3, step 3; most of it is written in NOW) | 1.5-2 | Decode times idle and during MP3 and Bluetooth; 0 underruns; the worker's stack and internal RAM unchanged |
| L7 | **End to end:** a card filled by the terminal on a PC, then booted | 0.5-1 | No scan of the software's files; the index equal to the host build's from the same card image; the listener's own album scanned; a pulled-card recovery |

**Needs a card, not the Core2** (can run before the Core2 is free, if
the user prepares the card and the machines):

- **C1:** the FAT-time recipes (2.3.4) on a real FAT32 card in a Windows,
  a Linux and a macOS reader, including a DST change: 0.5 day per OS.
- **C2:** N11's synthetic 20k card written by the user, ready for L0.

---

## 7. Open questions

### 7.1 For the user

- **U1. One card for the whole library?** If about 19,400 files will go on
  one card, can you prepare a FAT32 card for L0: the real library, or
  N11's synthetic one written from the PC?
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
- **U5. A software-owned file the listener edited:** keep it and report it
  (proposed), or overwrite it so the card mirrors the server?
- **U6. A card with music but no ledger:** adopt the files that equal the
  selection (same path and size, qfp checked) after asking, or always
  write beside them?
- **U7. A card filled from another mStream server:** refuse, ask, or take
  it over?
- **U8. Rescan tags:** should it override transfer records? Today a
  matching transfer record wins, so a retag that kept both the size and
  the time of a software-owned file stays invisible until the next
  transfer.
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
  extra update step, about 4-6 s at 20k), or only once their tags are
  read?
- **U12. If PSRAM is short at 20k** with a whole-library queue and
  AutoDJ: drop the undo snapshot for queues over about 5,000 entries
  first, or keep file names on the card instead of in PSRAM?
- **U13. The scan's battery floor:** 10% (proposed), lower, or a setting?
- **U14. May a host test vendor FatFs** (a download) to count sector reads
  on a synthetic 20k tree before L0?
- **U15. AutoDJ's table:** K = 100 (about 6 MB per 20k tracks on the
  card)? Rows only for embedded tracks, or also rows with filter data
  only (BPM, key) for tracks not yet embedded?
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
  later on the server so mStream's web panel can show it?
- **T4. The read-back:** will the software read names and FAT times back
  from the card with the per-OS recipes (2.3.4), and compute path hashes
  from the names read back, never from the names it meant to write?
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
2. **PSRAM at 20k** leaves about 0-0.35 MB in the worst case (a
   whole-library queue and AutoDJ). The levers are in 3.5; fragmentation
   can defer a build to the next boot.
3. **Sector-cache invalidation bugs would corrupt data.** L1's write soak
   comes before anything else ships.
4. **Two implementations of one format** (Rust and C++) can drift: the
   canonical order, path bytes (NFC and NFD), string limits. The golden
   files, byte-identical writer tests and the read-back rule cover it;
   the parity corpus covers the tag rules.
5. **mStream's rules move.** The album key, the year rule and the engines
   have changed between schemas. `readRules` versions the records; A3
   would carry the server's rules; the parity corpus pins this version.
6. **PC timestamps.** Time zones, DST and 2 s rounding are absorbed by
   the skew rule and qfp; the worst case is about 2.7 min of checks once.
7. **Long scans during Bluetooth playback** are unmeasured: the ring gate
   and the back-off are the guard, L3 the proof.
8. **A track end during the update step.** The safe-point rule (20 s left,
   no recent seek) covers it; Now Playing keeps its copy of the names.
9. **Retags the identity can't see:** a retag that keeps the size and the
   time, in the middle of the file. Rescan tags on the device, and the
   software's next run against the server's hashes, catch most.
10. **Licences.** The AutoDJ table is CC BY-NC-SA derived data (2.13.4);
    analysed BPM and key come from an AGPL library on the server, an
    owner question mStream already records; neither is code in the
    firmware.

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
