# AutoDJ index: `autodj-<gen>.bin` (MPDJ v2)

**Status (2026-10-13):** the format of record for the AutoDJ index that
the transfer software writes to the card, mstream-terminal first. It
replaces the MPDJ v1 draft that docs/METADATA.md 2.13 held, which never had
a reader. No released firmware reads this file yet: the player's reader and
writer are in `lib/core/CardAutoDj` (host-tested), and the AutoDJ engine
that will use them is a later PR (11.8). This document is minor 0; minor 1,
the cell-ordered codes of Appendix A, is planned. Every test vector (13) is
synthetic.

Reading a file needs nothing but this document, in any language, without
the player's code. Writing one also follows a few rules of the card
contract, docs/METADATA.md ("M" below), which 1.1 names; where this
document relies on one, it restates it and cites the section.

**What the user decided (2026-10-13):**

- **D1, the card space per track:** 512-byte slots of plain entries (a
  2-byte row index and a 1-byte level), 169 entries a slot, filled by the
  "long head" selection S84-55-169: the exact 84 nearest, then 85 entries at
  evenly spaced ranks down to the last candidate at level 140 or more
  (cosine 0.547 or more). The selection's kind, head length and floor are
  header fields, so a writer can change its defaults later and readers
  follow. The engine's two-hop fallback for "stay on seed" needs nothing in
  the file (11.5).
- **D2, mStream's exact DJ:** planned now, as the cell-ordered codes of
  Appendix A, a later minor version. The user relaxed the device's read
  budget to about 4 card reads a pick for AutoDJ's code path (the table
  path keeps 2), and approved one aggregate-only run on the server's real
  vectors to pin what depends on their spectrum. That run hasn't happened
  yet, so every part of Appendix A that depends on the real vectors is
  marked provisional.
- **D3, who computes the lists:** the terminal builds the file now; an
  mStream bulk-embeddings route may come later (M 4.2, A2). The file is the
  same either way.

---

## 0. In one screen

- `/.mstream/autodj-<gen>.bin` holds, for each recording on the card that
  has an embedding (a **row**), a list of up to k other rows by cosine
  similarity, with their scores. The transfer software writes it; the
  device only reads it.
- It is a card-contract container (M 2.4) with magic `MPDJ`, **major 2**,
  and four sections, in this order:

  | Section | Per | Bytes | What |
  |---|---|---|---|
  | `DJSL` | row | 256 (k 2-84) or 512 (k 85-169) | the row's list, in a fixed **slot** with its own CRC |
  | `DJRF` | row | 8 | BPM, Camelot key, a flag, artist key: the device's filters |
  | `DJPH` | card file | 10 | path hash → row |
  | `STRS` | file | small | model id, version, metric, licence, attribution |

- **One slot a pick.** The header is padded so that DJSL starts on a slot
  boundary: a pick reads `slotBytes` bytes at `DJSL.offset + row ×
  slotBytes`, one sector at most.
- **Each slot carries a CRC-32 seeded with its row,** so the device checks
  what it reads, needs no whole-file pass, and catches a read of the wrong
  row.
- **Plain entries:** a u16 row index and a u8 level, `level =
  clamp(round(cosine × 255), 0, 255)`. 84 entries fit 256 bytes and 169
  fit 512.
- **The neighbour rule is exact:** the f64 cosine in index order, quantised
  to a level first; the candidates ordered by (level descending, row index
  ascending); the row itself and the rows of its own song left out. Which
  candidates fill a list is the header's **selection**: top-k, or strided
  with an exact head (the writer's default: k 169, head 84, floor level
  140).
- **Two conforming writers produce the same bytes.** Equal selection
  signatures (6) mean equal slots.
- **Size at k = 169:** 530 bytes a track: 5.30 / 10.60 / 26.50 MB at 10k /
  20k / 50k tracks (10.2).

---

## 1. Scope and words

### 1.1 What this document is enough for

- **Reading** a file needs only this document.
- **Writing** one also follows the card contract (M) for what this file
  uses but doesn't define:
  - the commit and the root (M 2.5, 2.12.1), restated in 7.2;
  - the tags file's records, whose values DJRF and the song key take (M
    2.6.4-2.6.5, filled by M 2.7 and part 5);
  - how paths are spelled on the card, names with only an 8.3 entry
    included (M 2.8.3-2.8.5);
  - `/.player/device.txt` (M 2.15), restated in 7.4;
  - nameKey (M 5.4), restated in 2.4.
- MUST, SHOULD and MAY are used as in RFC 2119. Sections 11 and 12 and
  Appendices B and C are informative; Appendix A is informative until minor
  1 is published.

### 1.2 Words

- **Writer:** the software that copies music to the card and writes
  `/.mstream/` (mstream-terminal today; later perhaps the device as the
  sync agent, M 2.12.4).
- **Reader:** the player's firmware, or any program that reads the file.
  The device never writes it.
- **Row:** one distinct recording among the card files of the writer's
  selection that has an embedding, keyed by mStream's **canonical hash**
  (`audio-hash`, else `hash`: 32 lowercase hex digits, M 2.6.6). A row's
  **card files** are the files on the card with that hash.
- **Entry:** one place in a row's list: a row index and a level. The
  **used** entries come first; the rest are **unused** (index 0xFFFF).
- **Level:** an entry's u8 score, from the cosine (5.2).
- **Candidate:** a row that may stand in another row's list (5.3).
  **Rank:** a candidate's position in its row's order, from 0.
- **Head** and **tail:** the first selHead entries of a list (the exact
  top), and the entries after them.
- **Absent:** a reader behaves as if the file weren't on the card. AutoDJ
  then uses "random with filters" (M 2.13.4). It is never an error the
  listener must clear.
- **k, c, m, H, Lf:** the list length (a header field), a slot's used
  entries, a row's candidates at or above the floor, the head length
  (selHead) and the floor level (selFloor).

---

## 2. Conventions (M 2.3, restated)

### 2.1 Bytes

- Every integer is little-endian and unsigned.
- A **fourcc** is 4 ASCII bytes in reading order: `MPDJ` is the bytes
  `4D 50 44 4A`.
- **Byte strings** (the selection signature, a canonical hash, Appendix A's
  basisId and codesSig) are stored as the 16 bytes their 32 hex digits
  encode, first pair first: `af47…` is stored `AF 47 …`, never as ASCII
  text. They are compared with `memcmp`.
- Reserved fields, padding and the gaps between sections MUST be written as
  zero, and readers MUST ignore them (8.7).
- Text is UTF-8 with no NUL byte.

### 2.2 CRC-32

CRC-32/ISO-HDLC, as zlib, PNG and Rust's `crc32fast` compute it: reflected
polynomial 0xEDB88320, initial value 0xFFFFFFFF, final XOR 0xFFFFFFFF. The
CRC-32 of the ASCII bytes `123456789` is **0xCBF43926**. "CRC-32 over A,
then B" means one CRC run over A's bytes followed by B's: with zlib,
`crc32(B, crc32(A))`.

### 2.3 FNV-1a 64 and the path hash

- **FNV-1a 64:** offset basis 0xCBF29CE484222325, prime
  0x00000100000001B3; for each byte b, `h = (h XOR b) × prime`, mod 2^64.
- **The path hash** of a card file (M 2.3.3) is the FNV-1a 64 of the UTF-8
  bytes of its absolute card path: the literal lowercase `/music`, then `/`
  and each name below it, each spelled exactly as the card's FAT directory
  stores it (the stored long name as UTF-8, M 2.8.5); no trailing slash, no
  NUL. The device hashes the same bytes for its thumbnails and its Opus open
  cache.
- A writer MUST hash the names as the card stores them, not as its own OS
  lists them: the same text in NFC and in NFD has two hashes.

| Input | FNV-1a 64 |
|---|---|
| (empty) | CBF29CE484222325 |
| `/music` | 75DC8A6A38687865 |
| `/music/Artist/Album/01 - Title.mp3` | DA70B312760188D8 |
| `/music/Café/Album/01 - Title.mp3` (é as NFC, `C3 A9`) | F1B24FC757494F2B |
| the same with é as NFD (`65 CC 81`) | 57017C0CC011BB1B |

### 2.4 nameKey, the artist display, the song key, artistKey (M 5.4)

- **White_Space** is the Unicode property (Rust's `char::is_whitespace`):
  U+0009-000D, U+0020, U+0085, U+00A0, U+1680, U+2000-200A, U+2028, U+2029,
  U+202F, U+205F and U+3000. A string is **blank** when it is empty or holds
  only these.
- **nameKey(s):**
  1. each run of White_Space becomes one space, and the ends are trimmed;
  2. the quotes `‘ ’ ‚ ‛ ′` become `'`, and `“ ” „ ‟ ″` become `"`;
  3. the dashes `‐ ‑ ‒ – — ― −` become `-`;
  4. the string is lowercased by the Unicode default full mapping, with
     Final_Sigma (Rust's `str::to_lowercase`, Unicode 17.0's tables):
     `ΣΟΦΙΑΣ` gives `σοφιας`.

  No accent folding, no normalisation.
- **The artist display** of a list of artist values: each value trimmed of
  White_Space at both ends (the set above, not ASCII spaces alone), the
  blank ones dropped. One value left is the display as it is. Several are
  deduplicated by nameKey (the first of equal keys kept, in order) and
  joined with `, ` (a comma and a space). None gives the empty string. The
  trim matters: the values `A` followed by a no-break space, and `B`,
  display as `A, B` (nameKey `a, b`), where an ASCII trim keeps the
  no-break space and nameKey makes it `a , b`: another song key.
- **The song key** of a row: **none** when its title is blank (absent,
  empty or White_Space only). A title taken from the file name is never
  used. Otherwise it is `nameKey(artist display) + "|" + nameKey(title)`,
  compared as a string.
- **artistKey:** the low 32 bits of the FNV-1a 64 of the UTF-8 bytes of
  the nameKey of the **first non-blank** artist value; 0 when there is
  none. A device that keys a track without a row (for its artist
  cooldown) MUST use the same function of the record it chose for that
  track (M 2.13.2); a device need not key such tracks at all (the
  Core2's Auto DJ doesn't: they add nothing to its artist cooldown).

The title and the artist values are those of the row's primary file's
record (5.3).

| Input | Output |
|---|---|
| nameKey of `"  The Band’s  “Best” – Live "` | `the band's "best" - live` |
| nameKey of `ΣΟΦΙΑΣ` | `σοφιας` |
| the artist display of "Generated Artist 04", "Generated Artist 05", "generated␣␣artist 04" (two spaces) | `Generated Artist 04, Generated Artist 05` |
| the song key of "Generated Artist 01", title "Sample Title 01" | `generated artist 01\|sample title 01` |
| the song key with no artist value, title "Sample Café" | `\|sample café` |
| the song key with a title of three spaces, or none | none |
| the artistKey of "Generated Artist 01" (the FNV-1a 64 of `generated artist 01` is F8EEBFC1B460A770) | B460A770 |
| the artistKey of "␣␣␣" (three spaces), "Generated Artist 03" | B460AAD6, that of "Generated Artist 03" |

---

## 3. The container (M 2.4, restated)

### 3.1 The common header (bytes 0-39)

| Off | Size | Field | Value |
|---|---|---|---|
| 0 | 4 | magic | `MPDJ` |
| 4 | 2 | major | 2 |
| 6 | 2 | minor | 0 in this document (9.2) |
| 8 | 4 | headerBytes | the whole header: a multiple of 8, at least 104, chosen by 3.3. The directory starts here |
| 12 | 4 | sectionCount | the directory's entries, at most 64 (4 in minor 0) |
| 16 | 4 | fileBytes | the file's exact length |
| 20 | 4 | generation | the generation of the commit that wrote the file (7.2) |
| 24 | 8 | cardId | the card's id (M 2.5.2) |
| 32 | 4 | headerCrc | CRC-32 of the bytes `[0, headerBytes + 32 × sectionCount)`, these 4 taken as zero: the header, its padding and the directory, so every section's CRC too |
| 36 | 4 | reserved | 0 |

### 3.2 The directory and the layout

`sectionCount` entries of 32 bytes start at `headerBytes`:

| Off | Size | Field | Meaning |
|---|---|---|---|
| 0 | 4 | type | fourcc |
| 4 | 4 | flags | bit 0 REQUIRED: a reader that doesn't know this type MUST treat the file as absent. Other bits 0 |
| 8 | 4 | offset | from the file's start; a multiple of 8 |
| 12 | 4 | bytes | the section's length |
| 16 | 4 | count | records (0 for a blob) |
| 20 | 4 | stride | bytes a record (0 for a blob) |
| 24 | 4 | crc | CRC-32 of the section's bytes |
| 28 | 4 | reserved | 0 |

**The layout:**

- The sections lie in the directory's order. The first starts exactly at
  `headerBytes + 32 × sectionCount`; each next one at the end of the one
  before, rounded up to a multiple of 8. A gap (under 8 bytes) is zero, and
  no CRC covers it.
- The last section ends exactly at `fileBytes`.
- A type appears at most once.
- An array has `bytes = count × stride`; a blob has count = stride = 0.

### 3.3 MPDJ's alignment rule

- **headerBytes** is the smallest multiple of 8, at least the header's size
  (104 in minor 0; a later minor's larger header, 9.2), for which
  `headerBytes + 32 × sectionCount` is a multiple of `slotBytes` (4.1). The
  bytes from the header's end up to headerBytes are zero.
- DJSL is always the first section (4.2), so it starts on a slot boundary.

| sectionCount | slotBytes | Header size | headerBytes | DJSL at |
|---|---|---|---|---|
| 4 | 256 | 104 | 128 | 256 |
| 4 | 512 | 104 | 384 | 512 |
| 5 | 256 | 104 | 352 | 512 |
| 5 | 512 | 104 | 352 | 512 |
| 8 | 512 | 104 | 256 | 512 |
| 8 | 256 | 104 | 256 | 512 |
| 4 | 256 | 112 | 128 | 256 |

- A file starts on a FAT cluster, so on a 512-byte sector. A 256-byte slot
  at a multiple of 256 never crosses a sector, and a 512-byte slot at a
  multiple of 512 is exactly one sector.
- A writer MUST write the smallest headerBytes. Readers check only that it
  is a multiple of 8, at least 104, and that DJSL's offset is a multiple of
  slotBytes (8.1, 8.2), not that it is the smallest.
- The rule only chooses headerBytes: a generic reader of the card
  contract's container (M 2.4.3) reads these files as it reads any other.

---

## 4. The header and the sections

### 4.1 The header (bytes 40-103 in minor 0)

| Off | Size | Field | Value |
|---|---|---|---|
| 40 | 4 | rowCount | the rows, 1 to 65,535 |
| 44 | 4 | pathCount | DJPH's entries |
| 48 | 2 | k | the length of every list, 2 to 169; the writer's default 169 |
| 50 | 2 | slotBytes | 256 when k ≤ 84; 512 when 85 ≤ k ≤ 169 |
| 52 | 1 | indexBytes | 2 (row indexes are u16) |
| 53 | 1 | scoreKind | 1: the level of 5.2 |
| 54 | 1 | selKind | the selection (5.4): 0 top-k; 1 strided by rank, the writer's default |
| 55 | 1 | selHead | kind 0: k. Kind 1: H, 1 ≤ H ≤ k − 2 (default 84) |
| 56 | 4 | builtTime | Unix seconds (UTC) when the lists were computed; carried with DJSL (7.3); informational |
| 60 | 4 | tagsGeneration | the generation of the MPTG tags file DJRF was built from; diagnostic |
| 64 | 16 | selectionSig | 6, as bytes (2.1) |
| 80 | 4 | modelId | STRS offset: the embedding model's id, as the embeddings route's `model.id` gives it (`effnet-discogs` today) |
| 84 | 4 | modelVersion | STRS offset: its `model.version` (`1`) |
| 88 | 4 | metric | STRS offset: `cosine` |
| 92 | 4 | license | STRS offset: the licence of the data the lists derive from, the model's (below) |
| 96 | 4 | attribution | STRS offset: the model's attribution string (below) |
| 100 | 1 | selFloor | kind 0: 0. Kind 1: Lf, 1 to 255 (default 140) |
| 101 | 3 | reserved | 0 |
| 104 | … | padding | zeros up to headerBytes; a later minor's fields start here (9.2) |

- **slotBytes follows from k:** with 3 bytes an entry and a 4-byte CRC,
  (256 − 4) / 3 = 84 entries fit a 256-byte slot and ⌊(512 − 4) / 3⌋ = 169
  a 512-byte one (4.3). No other value is allowed.
- **k = 1 is refused:** a list of one is no DJ.
- **selKind 1 needs k ≥ 3** (H is at least 1 and at most k − 2): at k = 2
  a writer MUST use selKind 0, and below k = 86 the default H of 84
  doesn't fit, so it MUST choose an H of at most k − 2. A writer refuses
  a selection that 8.2 would reject.
- **Kind 0's selHead and selFloor are pinned** (k and 0), so every reader
  can use one rule for every kind: the first min(selHead, c) entries of a
  list are the exact top of its candidates (5.4).
- **The five strings** are the same for every writer, so that two writers
  make the same STRS (12.4) and nothing is rewritten for a string alone
  (7.3). modelId and modelVersion are the embeddings route's `model.id`
  and `model.version` (M 4.2). That route doesn't carry the licence or
  the attribution: they are mStream's model registry's for that model
  (its `src/db/discovery-features-lib.js`, READ by the research, Appendix
  C). For `effnet-discogs` version 1 they are, byte for byte:
  - license: `CC-BY-NC-SA-4.0`;
  - attribution: `Discogs-EffNet by Music Technology Group, Universitat
    Pompeu Fabra (essentia.upf.edu/models)` (92 bytes, one line, single
    spaces).

  With metric `cosine`, STRS is then 134 bytes (10.2). A model this
  document doesn't list has its strings added here in a revision; until
  then a writer takes them from the same registry, as given.

### 4.2 The sections and their order

| Type | Flags | Count | Stride | Content |
|---|---|---|---|---|
| `DJSL` | REQUIRED | rowCount | slotBytes | one slot a row, in row order (4.3) |
| `DJRF` | REQUIRED | rowCount | 8 | one filter record a row, in row order (4.4) |
| `DJPH` | REQUIRED | pathCount | 10 | card file → row (4.5) |
| `STRS` | REQUIRED | 0 | 0 | the strings (4.6) |

- DJSL is directory entry 0, and the four come in the order DJSL, DJRF,
  DJPH, STRS by directory position.
- A later minor's optional sections (REQUIRED clear) go between DJPH and
  STRS (minor 1's: Appendix A). Readers skip an unknown optional section
  wherever it stands after DJSL.

### 4.3 DJSL: the slots

Row r's slot is the `slotBytes` bytes at `DJSL.offset + r × slotBytes`.
With S = slotBytes:

| Slot offset | Size | Field |
|---|---|---|
| 0 | 2k | `index[0..k−1]`, u16 each, best first; 0xFFFF marks an unused entry |
| 2k | k | `level[0..k−1]`, u8 each; 0 for an unused entry |
| 3k | S − 4 − 3k | zero padding (k = 169: one byte, at 507; k = 84: none; k = 4: 240 bytes) |
| S − 4 | 4 | `crc`: CRC-32 over the 4 bytes of r as a u32 (little-endian), then the slot's bytes `[0, S − 4)` |

- **c,** the used entries, is the position of the first 0xFFFF index, or k
  when there is none. Entries c to k − 1 are unused: index 0xFFFF, level 0.
  Unused entries come only after every used one.
- Entries 0 to c − 1 are the row's list (5.4), best first: the levels never
  rise, and among equal levels the row indexes increase.
- **The seed** makes a slot read for the wrong row fail its CRC. With zlib:
  `crc32(slot[0 : S−4], crc32(r as 4 bytes, little-endian))`.
- **DJSL's section CRC says nothing the slot CRCs don't.** A CRC-32 run
  over data followed by that data's own CRC ends in a fixed state, so the
  section CRC of a DJSL whose slots all pass their CRCs depends only on
  rowCount and slotBytes: it is 8703FDFE for any 6 such slots of 256
  bytes, whatever they list. It still catches damage, and a slot at the wrong
  row, in one check (8.5). But it can't tell apart two files whose slots
  differ under good CRCs, and so neither can headerCrc nor the root's COMP
  entry: the selection signature (6) is what names the lists.

### 4.4 DJRF: the filters (8 bytes a row, in row order)

| Off | Size | Field |
|---|---|---|
| 0 | 2 | bpm10: the record's `bpm10` (BPM × 10, as stored; the writer doesn't round anything); 0 none |
| 2 | 1 | camelot: the record's `camelot`: 0 none, 1-12 = 1A-12A (minor), 13-24 = 1B-12B (major). A writer writes a value above 24 as 0; a reader reads one as 0 |
| 3 | 1 | flags: bit 0 BPM_ANALYSED, the record's flags bit 8 (bpm10 and camelot are mStream's analysis, not the file's tags). Other bits 0; readers ignore bits they don't know |
| 4 | 4 | artistKey (2.4) of the record's artist values; 0 none |

The record is the row's primary file's (5.3). A later minor may make the
record longer; readers read the 8 bytes they know (9.2).

### 4.5 DJPH: card file → row (10 bytes an entry)

| Off | Size | Field |
|---|---|---|
| 0 | 8 | pathHash (2.3), u64 |
| 8 | 2 | row, u16 |

- Every card file of every row appears once, sorted by pathHash, strictly
  increasing (each hash once). A recording at several paths has several
  entries with one row.
- **Collisions:** a path hash that two or more card files share (a 64-bit
  FNV collision, within one row or across rows) is left out of DJPH
  entirely. A row left with no entry stays in the table: it can be a
  neighbour, never an anchor. So pathCount may be below rowCount, and may
  be 0.
- **An empty DJPH** (pathCount 0) is still an array: its directory entry
  has bytes 0, count 0 and stride 10, and its CRC is 0 (the CRC-32 of no
  bytes). A stride of 0 there makes the file absent (shape, 8.1), as for
  any array. 13.6's mpdj-paths-empty and mpdj-paths-empty-stride-zero hold
  both.
- A later minor may make the entry longer; readers read the 10 bytes they
  know.

### 4.6 STRS: the strings (M 2.4.4)

- A blob of NUL-terminated UTF-8 strings. Byte 0 is NUL, so offset 0 is the
  empty string.
- Then each non-empty string of the header, with its NUL, in header-field
  order: modelId, modelVersion, metric, license, attribution. Nothing else
  in minor 0; a later minor's string fields follow, in their header order.
- A string holds no NUL and is at most 255 bytes: a writer rule, which
  readers don't check (they bound their copies instead, 8.3). Strings are
  never shared between fields; an empty string is offset 0 and is not
  written.

---

## 5. The neighbour lists

### 5.1 Rows and their order

- **The rows** are the distinct canonical hashes among the card files of
  the writer's selection that have an embedding: one row a hash.
- **The order:** by the canonical hash's 16 bytes, ascending (`memcmp`). A
  row's index is its position. Every list, every index and the signature
  depend on this order.
- A selection with no embedded recording, or with more than 65,535, has no
  MPDJ (7.3, 10.1).

### 5.2 The cosine and the level

- **The embeddings** are mStream's (`/api/v1/discovery/local/embeddings`,
  or a later bulk route, M 4.2 A2): `dim` f32 values a row, 1,280 for
  `effnet-discogs` version 1, used as the server sends them and never
  normalised again. Every row has the same dim; a writer refuses mixed
  dims.
- **The f32 values:** raw little-endian f32 bytes (the embeddings route's
  base64, a bulk route, a `.f32` file) are used as they are. A value given
  as a decimal number (JSON text, 13.2's descriptions) is read as the
  nearest f64 and then rounded to the nearest f32, ties to even (C's
  `(float)` of a `double`; a value out of f32's range or not finite is
  refused). Reading the decimal straight to f32 can differ by a double
  rounding; this two-step reading is the rule.
- **cos(a, b)** is the sum, in index order 0 to dim − 1, of the products of
  the two rows' f32 components, each product and each partial sum computed
  in f64. A product of two f32 values is exact in f64, so a fused
  multiply-add gives the same sum; only the order matters. (Informative:
  mStream's own similarity sums the same way, READ by the research in its
  `src/db/discovery-similarity.js`: f32 values multiplied and accumulated
  in a JavaScript number, an f64, in index order.)
- **The level** is `clamp(round(cos × 255), 0, 255)`: one f64 multiply,
  rounded half away from zero (C's `round()`, not banker's rounding); a
  negative value gives 0.

| cos | cos × 255 | Level |
|---|---|---|
| 1.0 | 255 | 255 |
| 0.75 | 191.25 | 191 |
| 0.5 | 127.5 | **128** (half away from zero) |
| 0.390625 | 99.609375 | 100 |
| 0.25 | 63.75 | 64 |
| 0.0 | 0 | 0 |
| −0.5 | −127.5 | 0 |
| 0.5470588389514506 (13.5's float32 trap, by the rule) | 139.5000039… | 140 |
| 0.5470588207244873 (the same pair through a float32 sum) | 139.4999993… | 139 |

### 5.3 The candidates, and where the keys come from

- **The primary file** of a row is its card file with the smallest path
  hash, as a u64; among files of one row whose hashes are equal, the one
  whose path's UTF-8 bytes are smaller (`memcmp`). A file whose hash a
  collision left out of DJPH (4.5) can still be the primary file.
- **Its record** is the primary file's RECS record in the MPTG tags file of
  the same commit (M 2.6.4-2.6.5). From it come:
  - the title: the string run's field 0, the first title value as stored
    (untrimmed);
  - the artist values: field 1, the list separated by U+001F;
  - DJRF's bpm10, camelot and BPM_ANALYSED (flags bit 8).

  A record with no tags (NO_TAGS, UNREADABLE) has no title, so its row has
  no song key.
- **The candidates of row a** are every row b ≠ a, except the rows whose
  song key equals a's. Both must have one: a row without a song key
  excludes nothing and is excluded by nothing. A candidate at level 0 is
  still a candidate. n_a is their number.
- **Their order** is by level descending, then by row index ascending. It
  is a total order, so no sort's stability is involved. A candidate's
  **rank** is its position in it, from 0.

### 5.4 The selection

k comes from the header; c is the number of used entries (4.3).

**selKind 0, top-k:** c = min(k, n_a), and entry j is the candidate at rank
j.

**selKind 1, strided by rank,** with H = selHead and Lf = selFloor:

1. m is the number of candidates at level Lf or above; they are ranks 0 to
   m − 1.
2. If m ≤ k, the list is the top min(k, n_a), exactly as kind 0. It may
   then hold candidates below Lf.
3. Otherwise c = k, and entry j is the candidate at rank
   - r_j = j, for 0 ≤ j < H;
   - r_j = H + ⌊(j − H) × (m − 1 − H) / (k − 1 − H)⌋, for H ≤ j < k,

   in integer arithmetic of at least 32 bits (the product is below 2^24).
   So r_H = H, r_(k−1) = m − 1, the last candidate at or above Lf,
   and the ranks strictly increase: the list stays in candidate order.

The formula is exact as written. Two readings one symbol away change the
synthetic test card's lists on 96 % of its rows: Lf = 141 for "cosine 0.55"
(140 / 255 = 0.549 is below 0.55), and the stride H + ⌊(j − H)(m − H) /
(k − H)⌋.

**The writer's default** is selKind 1 with k = 169, H = 84 and Lf = 140,
named **S84-55-169**: the exact top 84, then 85 entries at evenly spaced
ranks down to the last candidate at level 140 or more (cosine ≥ 139.5 / 255
= 0.547). Lf = 140 is mStream's default floor, 0.55, as `⌊F × 255 + 0.5⌋`
(informative: a writer may write any Lf).

**What a reader may rely on,** for every kind, unknown ones included (9.3):

- every entry is a candidate (5.3), and the list is in candidate order;
- the first min(selHead, c) entries are the exact top of the row's
  candidates;
- for kind 1, the entries after H are an evenly spaced sample of the row's
  candidates at or above Lf (rows with m ≤ k are plain top-k).

Readers check what a slot alone can show (8.4: no self, indexes in range,
the order); the rest only a recomputation can tell (8.7).

**Ranks** (vectors.json `mpdj.stridedRanks`):

| k | H | m | Ranks |
|---|---|---|---|
| 4 | 1 | 5 | 0, 1, 2, 4 |
| 24 | 8 | 100 | 0-7, then 8, 14, 20, … 92 (every 6th), then 99 |
| 169 | 84 | 170 | 0-167 and 169: rank 168 is skipped |
| 169 | 84 | 2,258 | r_84 = 84, r_85 = 109, r_126 = 1,170, r_168 = 2,257 |
| 169 | 84 | 169 | 0-168: m ≤ k, the top k |

### 5.5 A worked example: autodj-00000029's row 0

The file is 13.4's. Its k is 4, H 1 and Lf 100. Row 0's dot products × 256
with rows 1-5 are 100, 201, 100, 163 and 186, so its levels are 100, 200,
100, 162 and 185 (201 × 255 / 256 = 200.22 gives 200; 100 × 255 / 256 =
99.61 gives 100). No other row shares its song. Its candidates in order:

| Rank | 0 | 1 | 2 | 3 | 4 |
|---|---|---|---|---|---|
| Row / level | 2 / 200 | 5 / 185 | 4 / 162 | 1 / 100 | 3 / 100 |

- Rows 1 and 3 tie at level 100: the lower index, 1, ranks first.
- m = 5 (every candidate is at level 100 or more), which is more than k =
  4, so the list is strided: r_0 = 0, then r_j = 1 + ⌊(j − 1) × 3 / 2⌋ = 1,
  2, 4 for j = 1, 2, 3.
- The slot holds 2/200, 5/185, 4/162, 3/100. Rank 3 (row 1) is skipped, and
  rank 4, the last at or above Lf, is row 3, because the tie order put it
  last.
- A top-k list (autodj-small-top, selKind 0) keeps rank 3 instead: 2/200,
  5/185, 4/162, 1/100.

---

## 6. The selection signature

selectionSig is the first 16 bytes of the SHA-256 of this UTF-8 text, every
line (the last included) ended by one LF (0x0A), numbers in decimal without
leading zeros:

```
MPDJ-SEL 2
model <modelId> <modelVersion>
metric <metric>
k <k>
sel top                          (selKind 0)
sel strided <H> <Lf>             (selKind 1)
vec <dim> <64 lowercase hex>     the SHA-256 of every row's dim f32 values, little-endian, in row order
<canonical hash of row 0>        32 lowercase hex digits, one line a row, in row order
...
same <i> <j> ...                 one line a same-song group
```

- **The fields** are written as the header's strings hold them, one space
  between, even when one is empty: an empty modelVersion gives the line
  `model effnet-discogs ` (the space kept before the line's LF).
- **A same-song group** is two or more rows that share a song key (rows
  without one never group). Its line lists the members' row indexes,
  ascending, separated by single spaces. Groups are ordered by their first
  member. With no group there is no `same` line.
- **The text covers everything DJSL depends on:** the rows and their order,
  their vectors, the model, the metric, k, the selection and the same-song
  exclusions. So equal signatures mean equal DJSL bytes, by construction. A
  re-analysis under one model version changes the vectors, and so the
  signature: the vectors are not taken to follow from (hash, model,
  version).
- A later selKind defines its own `sel` line (9.3).
- **The device never computes it.** It compares the header's 16 bytes with
  the root's COMP `key` (7.2).

**Example:** autodj-00000029 (13.4) has the text

```
MPDJ-SEL 2
model synthetic-embedding 1
metric cosine
k 4
sel strided 1 100
vec 8 038912838c44e3d8a2814ac75a927d6bf32122302c0ff5fca53f52f623a2370f
0123456789abcdef0123456789abcdef
1c3e5a7092b4d6f81c3e5a7092b4d6f8
5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a
9e1f00000000000000000000000000a1
b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0
fedcba9876543210fedcba9876543210
same 3 4 5
```

whose selectionSig is **af4752829aa50afa2b5f2e6aecc56d59**, stored at bytes
64-79 as `AF 47 52 82 9A A5 0A FA 2B 5F 2E 6A EC C5 6D 59`. Its `vec` line
is the digest of 13.4's 6 × 8 f32 values in row order.

---

## 7. On the card

### 7.1 Name and place

- `/.mstream/autodj-<gen>.bin`, where `<gen>` is the header's generation as
  8 lowercase hex digits: `autodj-00000029.bin` is generation 41.
- It is optional: a card with none gets random with filters (M 2.13.4).
- Only the transfer software writes it. The device never writes
  `/.mstream/` (M 2.11).

### 7.2 The commit (M 2.5.3, 2.12.1)

A file counts only when the card's root, `/.mstream/manifest.bin` (MSMF),
names it in its COMP section: after the root's one MPTG entry, at most one
MPDJ entry of 40 bytes.

| Off | Size | Field | Value |
|---|---|---|---|
| 0 | 4 | kind | `MPDJ` |
| 4 | 4 | generation | the file's header generation (older than the root's when a root names the file again) |
| 8 | 4 | fileBytes | its length |
| 12 | 4 | headerCrc | its headerCrc |
| 16 | 4 | name | the root's STRS offset of `autodj-<gen>.bin` |
| 20 | 4 | reserved | 0 |
| 24 | 16 | key | its selectionSig |

- **The file is valid** only when the root names it, it validates (8), its
  generation, fileBytes and headerCrc equal the entry's, and its
  selectionSig equals the entry's key. Otherwise it is absent, and the root
  stays valid.
- **The writer's order in a run:** write `autodj-<g>.bin` under its final
  name (nothing names it yet, so a cut leaves the old state); then the root
  through `manifest.tmp` (delete `manifest.bin`, then rename); then delete
  the companions the new root no longer names.
- The fixture card's root (`test/fixtures/card/golden/manifest.bin`,
  generation 42) names autodj-00000029.bin, generation 41: a file carried
  from the commit before.

### 7.3 When to write a new file

At each run the writer works out what it would write now: the signature,
DJRF, DJPH and STRS (the lists only when they are needed). Then:

| Against the file the current root names | Then |
|---|---|
| device.txt's `read.mpdj` doesn't list 2 (7.4) | no MPDJ: the new root names none |
| no file, or another signature (rows, vectors, model, metric, k, selection, same-song groups) | compute the lists and write a new file |
| the same signature, but DJRF, DJPH or STRS would change (a retag, a moved or renamed file, a new path for a recording) | write a new file, its header values from this commit (generation, cardId, tagsGeneration). It MAY copy the old DJSL bytes unchanged, since slots depend only on what the signature covers and their CRCs on the row; it then keeps their builtTime |
| nothing changes | the new root names the old file again; its generation stays older than the root's |

Minor 1 adds a row: the same signature, but the code sections would change
(A.5). `python tools/mpdj.py restamp OLD.bin DESC.json OUT.bin` makes the
copy, and refuses when the signatures differ.

### 7.4 device.txt (M 2.15)

- A writer writes MPDJ v2 **only when `/.player/device.txt`'s `read.mpdj`
  lists 2** (a comma-separated list of majors). Otherwise it writes no
  MPDJ: no reader of v1 exists, so there is nothing older to write
  instead.
- No device.txt, or no `read.mpdj` key in it, reads as none.
- Released firmware 0.8.0-0.10.0 writes `read.mpdj=1`, yet reads no MPDJ:
  no MPDJ for them. From 0.11.0 the firmware writes `read.mpdj=` (none)
  until its AutoDJ engine ships, then `read.mpdj=2`.

| `read.mpdj` | Writes MPDJ v2? |
|---|---|
| (empty) | no |
| `1` | no |
| `2` | yes |
| `1,2` | yes |
| no device.txt | no |
| the key missing | no |

---

## 8. Validation

A reader MUST treat the file as absent when a check of 8.1-8.4 fails, with
one exception: a slot's own CRC, under 8.5's rule. A check that needs a
whole section MAY run while the section streams; a failure found then makes
the file absent from then on. The words in parentheses are the reference
reader's reasons (8.6, 13.6).

### 8.1 The frame (M 2.4.3), in this order

1. The file holds at least 40 bytes (short).
2. magic is `MPDJ` (magic).
3. major is 2. A reader of this document reads {2}; any other major makes
   the file absent (major, 9.1). Any minor is accepted.
4. headerBytes is a multiple of 8 and at least 104 (headerBytes).
5. sectionCount ≤ 64 (sectionCount).
6. headerBytes + 32 × sectionCount ≤ the file's length (short).
7. fileBytes equals the file's length (fileBytes).
8. headerCrc matches (headerCrc).
9. The directory, entry by entry: the offset is a multiple of 8, at or
   after the end of the section before it (the directory's end for the
   first), less than 8 bytes past that end, and offset + bytes ≤
   fileBytes; the type hasn't appeared before (layout). Then an unknown
   type with REQUIRED set makes the file absent (unknownRequired), and one
   without it is skipped. For a known type: STRS has count = stride = 0,
   and an array has a stride of at least its minor-0 stride (DJSL 256, DJRF
   8, DJPH 10) and bytes = count × stride (shape).
10. The last section ends at fileBytes (layout).
11. DJSL, DJRF, DJPH and STRS are all present (missing).

**Without overflow:** the sums and the product of steps 6, 9 and 10
(headerBytes + 32 × sectionCount, offset + bytes, count × stride) MUST be
computed without wrapping: in 64 bits, or with an explicit check. Each of
them can pass 2^32 with u32 fields, and wrapped to 32 bits it lets a
crafted directory through: 13.6's mpdj-header-bytes-wrap,
mpdj-section-end-wrap and mpdj-array-bytes-wrap are such files (a 32-bit
sum reads the second as present).

### 8.2 The header, in this order

1. 1 ≤ rowCount ≤ 65,535 (enum).
2. 2 ≤ k ≤ 169 (enum).
3. slotBytes is 256 when k ≤ 84 and 512 when k ≥ 85 (enum).
4. indexBytes is 2 and scoreKind is 1 (enum).
5. The selection (djSelection):
   - selKind 0: selHead = k and selFloor = 0;
   - selKind 1: 1 ≤ selHead ≤ k − 2 and 1 ≤ selFloor ≤ 255;
   - any other selKind: selHead ≤ k (selFloor any value).
6. DJSL's and DJRF's counts equal rowCount, and DJPH's equals pathCount
   (counts).
7. DJSL's stride equals slotBytes (shape).
8. DJSL is directory entry 0, its offset is a multiple of slotBytes, and
   DJSL, DJRF, DJPH and STRS come in that order by directory position
   (djLayout).

Not checked: the reserved bytes 101-103, the bytes from 104 up to
headerBytes (headerCrc covers them), builtTime and tagsGeneration.

### 8.3 STRS, DJRF and DJPH

- **STRS,** always, before any string is used:
  1. byte 0 is NUL (string);
  2. for each of the five string offsets in header-field order that isn't
     0: it lies inside STRS (string); it is at or after the end of the
     string of the previous offset that isn't 0, its NUL included, or at
     or after 1 for the first such offset (stringOrder; an empty string
     between them is skipped, and gaps are allowed, for a later minor's
     strings); a NUL follows it inside STRS (string);
  3. the section's CRC (sectionCrc).

  A reader copies at most its buffer's size − 1 bytes of a string.
- **DJRF:** its CRC (sectionCrc). The device checks it as it streams at the
  join (11.2).
- **DJPH:** each entry's row is below rowCount, and the path hashes
  strictly increase (djPaths); then the section's CRC (sectionCrc). A row
  with no DJPH entry is allowed.

### 8.4 A slot

Each slot a reader uses is checked in this order, each check over the
whole slot before the next:

- **S1.** Its CRC (4.3) matches, seeded with the row the reader asked for
  (djSlotCrc).
- **S2.** After the first 0xFFFF index, every index is 0xFFFF
  (djSlotUnused).
- **S3.** Every used index is below rowCount and isn't the row itself
  (djSlotIndex).
- **S4.** For 0 < j < c: level[j] ≤ level[j − 1], and when level[j] =
  level[j − 1], index[j] > index[j − 1] (djSlotOrder).

A reader MAY also check that no index appears twice in a slot, with a
rowCount-bit map. A writer never writes one twice.

### 8.5 Absent, or one slot bad

- **A failure in 8.1-8.3** makes the file absent: AutoDJ uses random with
  filters. The device MAY note the file in `/.player` by (generation,
  headerCrc), so as not to try it again.
- **A reader that verifies the whole file** (a writer's own check, a host
  tool, 8.6's "all") treats any slot failure as absent.
- **A reader that checks slots as it reads them** (the device):
  - S1 fails: the slot is **damaged** (the card's fault, not the writer's).
    Only that slot is unusable: its anchor gets random with filters for
    this pick, and a hop through it is skipped. The reader MAY read it once
    more, and goes on with the file.
  - S2, S3 or S4 fails under a good CRC: the slot is **broken**, the work
    of a broken writer. The file is absent from then on.
- Instead of the slot CRCs a reader MAY check DJSL's section CRC once
  (4.3). A mismatch then sends it back to the slot CRCs, never to "absent"
  by itself.

### 8.6 The check order

So that every reader gives the same first reason, a reader that verifies
runs, in this order:

1. 8.1 and 8.2 (opening the file);
2. STRS (8.3), always;
3. DJRF's CRC, when it uses the filters;
4. DJPH (8.3), when it uses the paths;
5. when it uses the slots: each slot in row order, S1-S4 each, and last
   DJSL's section CRC.

The first failure is the reason. The reference tools have two sets of
uses: **all** (filters, paths and slots: a whole-file verification) and
**device** (filters and paths: the join; the slots are checked as they are
read, 8.5).

### 8.7 Not checked by readers (writers MUST get them right)

- That the lists follow section 5: only a recomputation can tell.
- That unused entries' level bytes, slot padding, the gaps between
  sections and the header's padding are zero. The slot CRCs cover the
  first two, so a writer that leaves garbage there still writes a file
  every reader reads, but not the bytes another writer makes.
- That DJRF matches the tags file, that every row has a DJPH entry, that no
  string is longer than 255 bytes.

---

## 9. Versions

### 9.1 Major

- **Major 2** is this document. A reader that doesn't support a file's
  major treats the file as absent.
- **Major 1** was the v1 draft of docs/METADATA.md 2.13 (2026-10-07). It
  never had a reader, and no software writes it (Appendix B). A v2 writer
  knows it as an older major, which it replaces.
- **The writer's guard** (M 2.4.5): a writer MUST NOT write to a card where
  any contract file in `/.mstream` carries a known magic with a major it
  doesn't know. For MPDJ, a writer of this document knows majors 1 and 2.
- **These need a new major:** a change to the slot's layout, the neighbour
  rule (5.2-5.3) or the row order; any existing field's meaning or
  position; an ordering rule readers rely on (4.2's section order, 8.4's
  list order); a new REQUIRED section.

### 9.2 Minor

A reader accepts any minor. A minor only adds, every addition MUST be
re-derivable from the card and the server (M 2.4.5), and all-zero bytes
mean "absent". A minor may add:

- header fields at offset 104 and later (the alignment rule, 3.3, then
  applies to the larger header);
- longer DJRF and DJPH records (a larger stride; readers read the prefix
  they know);
- optional sections (REQUIRED clear) between DJPH and STRS;
- DJRF flag bits;
- selKind values (9.3);
- string fields after attribution, their strings after attribution's in
  STRS.

DJSL can't change within major 2: its layout follows from k.

### 9.3 scoreKind, selKind and the model

- **scoreKind** 1 is the only kind. Any other value makes the file absent:
  another similarity would change every level.
- **selKind:** a reader uses only selHead and selFloor (5.4's guarantees),
  so an unknown kind (2 or more, still checked by 8.2) doesn't make the
  file absent. A new kind needs a new minor and an update of 5.4 and of
  6's `sel` line.
- **The model:** a new model or model version changes the signature (6),
  so new lists are written. Readers never interpret modelId; they may show
  it.

### 9.4 Minor 1, planned

Minor 1 is to add the cell-ordered codes of Appendix A: four optional
sections between DJPH and STRS. Nothing in minor 0 depends on it, and a
minor-0 reader reads a minor-1 file's table unchanged (13.6's
mpdj-minor-1 tests that path).

---

## 10. Limits and sizes

### 10.1 Limits

| Item | Limit |
|---|---|
| rowCount | 1 to 65,535 (u16 indexes; 0xFFFF is the unused mark). The device's PSRAM tops out near 23,000-27,000 tracks (M 3.5), so no device reads a bigger file; a selection with more rows has no MPDJ in major 2 |
| k | 2 to 169; the writer's default 169 |
| slotBytes | 256 (k 2-84) or 512 (k 85-169) |
| selHead, selFloor | u8 each: a list longer than 255 entries would need a new field |
| pathCount | any; normally the number of card files with a row, fewer after a collision (4.5) |
| a string | at most 255 bytes |
| the file | under 4 GiB (u32 offsets); in practice at most 35 MB (10.2) |

### 10.2 Size

A minor-0 file's length follows exactly from its layout. With n rows, p
DJPH entries and s bytes of STRS:

```
size = slotBytes + n × slotBytes + 8n + 10p + pad + s
```

- The first term is the header and the directory: 256 or 512 bytes with
  four sections (3.3).
- pad is the gap before STRS, (8 − (2p mod 8)) mod 8 bytes: DJSL's and
  DJRF's lengths are multiples of 8, and DJPH's is 10p.
- At one path a row that is 512 + 530n + pad + s with 512-byte slots, and
  256 + 274n + pad + s with 256-byte slots.
- `python tools/mpdj.py size ROWS [--paths P] [--k K] [--strings S]`
  computes it.

At one path a row, with s = 134 (the five strings of `effnet-discogs`
version 1, 4.1):

| Rows | 512-byte slots (k 85-169) | 256-byte slots (k 2-84) |
|---|---|---|
| 10,000 | 5,300,646 B (5.30 MB) | 2,740,390 B (2.74 MB) |
| 20,000 | 10,600,646 B (10.60 MB) | 5,480,390 B (5.48 MB) |
| 50,000 | 26,500,646 B (26.50 MB) | 13,700,390 B (13.70 MB) |
| 65,535 | 34,734,198 B (34.73 MB) | 17,956,982 B (17.96 MB) |

- At 20k, DJSL is 10.24 MB of the 10.60, and the join's sections (DJRF,
  DJPH and STRS) 0.36 MB.
- The synthetic test card's index (19,410 rows with one path each and 86
  bytes of strings, 13.7) is 10,287,902 bytes.
- **10.6 MB at 20k is over the device budget's soft 10 MB** (H14,
  Appendix C). That is harmless: the limit was there to keep a whole-file
  check under 8 s, and v2 has none. A WiFi resend of the file takes about
  53 s at 0.2 MB/s and 15 s at 0.7 MB/s (ESTIMATED); a PC's card reader,
  about a second.

---

## 11. What the device does (informative)

Nothing here is part of the format. It is the engine the format was
designed for, so that a reader knows what the fields are for. Device
figures are ESTIMATED at 20,000 tracks from the Core2's measured card and
CPU rates unless labelled; session figures are MEASURED-SYN on the
synthetic test card (Appendix C).

### 11.1 At boot

- One small read: the header and the directory are the file's first 256 or
  512 bytes (3.3). The device checks them (8.1, 8.2) and the root's COMP
  entry (7.2).

### 11.2 The join (after each library build: the boot's and the update step's)

- One sequential pass over DJRF, DJPH and STRS: 18 bytes a track, 360,000
  bytes at 20k, CRC-checked as it streams (8.3).
- It builds **track → row** and **row → track,** a u16 each. Each library
  track's path hash, computed from its path, is merged with DJPH. A row
  whose files are all gone stays unmapped. For a row with several files on
  the card, row → track takes the present file with the smallest path hash
  (the first DJPH entry of that row that maps to a track).
- And **the resident filters** from DJRF: the BPM, the Camelot key and the
  artistKey, as compact as the device likes.
- **Its transient,** a sorted array of (path hash, track) pairs at 12 bytes
  a track, is 233-240 KB at 19-20k tracks, against the device budget's
  limit of about 256 KB for one allocation (H5). It passes that limit above
  about 21,000-22,000 tracks, inside the device's 23-27k ceiling, so the
  engine must build the join in pieces (11.8).
- **No whole-file check of DJSL:** AutoDJ may use the file as soon as the
  join ends.

### 11.3 One pick, and the read path

1. The anchor's row, from track → row. A track with no row gets random
   with filters for this pick.
2. **One read:** slotBytes bytes at DJSL.offset + row × slotBytes.
3. The slot checked (8.4, 8.5) and decoded: about 7-8 k cycles for a
   512-byte slot (the CRC about 10-12 cycles a byte), tens of
   microseconds.
4. Merged with the cached slots of the session's anchors (the last few
   picks, or the seed), then gated (11.4), filtered (cooldowns, the BPM and
   key tiers, the artist key, from the resident DJRF data) and drawn; the
   chosen row mapped to a track. Rows that map to no track are skipped.

**The read path matters more than the layout:**

- "One card command a pick" holds only on the raw FatFs path: a `FIL` kept
  open, with the file's FAT-chain sectors in the sector cache. Fast seek is
  off on the device, so a cold seek walks the chain: about 5 FAT sectors
  for a 10.6 MB file with 16 KB clusters.
- Through Arduino's `File` (the VFS), a read costs about 3 ms plus 0.65 ms
  a KB whatever the layout (MEASURED fit), so the slot layout's gain exists
  only on the raw path. The firmware should read slots on the raw path, and
  never through the card worker's queue (H9: one of its jobs can hold it
  for seconds).
- Each pick puts one sector into the 256-sector cache the card shares with
  `/music`'s directories.

### 11.4 The gate and the slider

- **The gate** (engine policy): a candidate passes when its best listed
  level to any recent anchor reaches the floor's level. Never gate on a
  centroid estimate built from slot levels: a slot holds a few percent of
  its row's pool, so "not listed" says almost nothing, and such a gate
  either passes everything or collapses into one cluster (MEASURED-SYN).
- **A floor t passes level L when L ≥ ⌈t × 255⌉,** that is when L ≥ t ×
  255 exactly. That is what every simulation measured. With the slider in
  hundredths p, the least level is `(255 × p + 99) div 100`, in integers:

  | Slider | 0.20 | 0.30 | 0.45 | 0.55 | 0.65 | 0.75 | 0.85 | 0.90 |
  |---|---|---|---|---|---|---|---|---|
  | Least level | 51 | 77 | 115 | 141 | 166 | 192 | 217 | 230 |

  A float slider can be one level off where t × 255 is an integer:
  f32(0.6) × 255 = 153.000006, which gives 154, not 153. The integer form
  needs no such luck.
- **The engine clamps the floor level to at least selFloor** (140 by
  default, the slider at 140 / 255 = 0.549). Below it every listed tail
  entry would pass, so the slider is live only from selFloor / 255 up: mStream's band from 0.30 to
  0.55, 42 % of its slider, does nothing on the device. The clamp is also
  what keeps "looser than 0.55 plays like 0.55" true in seed mode: without
  it, two-hop (11.5) at 0.45 cuts BPM/key breaks to 1.2 % (MEASURED-SYN).
- **The slider works from 0.55 to about 0.75** (the pick cosine's span is
  0.099, against mStream's 0.149). At 0.85 every table puts 39-43 % of
  rolling picks below the floor, where mStream puts 2.7 %.
- **Honesty:** with the default lists 6.5 / 9.4 / 18.6 % of rolling picks
  land below the floor at 0.55 / 0.65 / 0.75, about half of them within
  0.05 (mStream: 0 / 0.1 / 0.4 %, 11.7). At 0.75 every table, tight or
  not, puts 16-22 % there (MEASURED-SYN).
- **When the gate passes nothing:** relax to the 50 best-ranked listed
  tracks and keep playing; never stop or go silent. (Round 1 asked the user
  this; it is engine policy now.)
- **"More like this"** and any ranked list are exact for the first selHead
  entries. An engine may play the tight DJ from the head alone: S84's head
  is the plain top-84 table.

### 11.5 Two-hop in "stay on seed"

- With the default lists, seed mode needs a second read at times. When the
  seed's pool has no tier-0 BPM/key row, or fewer than 4, or nothing left
  after the gate and the cooldowns, the engine reads the slots of up to 2 −
  (the anchor slots read this pick) intermediate rows j, drawn from the
  first selHead entries of the anchor's slot (j never an anchor), and draws
  from the union of the anchor's list and the j lists.
- A row reached only through j is gated on q(a, j) × q(j, c) / 255², which
  reads about 0.05 low, with 91-96 % precision; a row an anchor lists keeps
  its exact best listed level. Never more than 2 reads a pick. (The
  research calls it `TH.UN.P.BUD`.)
- Nothing of it is in the file: j's slot is an ordinary slot.
- **The default lists need it** for their seed continuity. Without it seed
  mode breaks BPM/key continuity on 23 / 47 / 58 % of picks at 0.55 / 0.65
  / 0.75; with it, on 7 / 30 / 53 % (MEASURED-SYN).
- The j rule is untuned: "uniform over the top 20" beat "uniform over the
  head" by 0.7 / 2.5 / 1.2 points and 0.2-0.4 reads a pick (an engine item,
  11.8).

### 11.6 Costs at 20,000 tracks (ESTIMATED)

| | Rolling ("follow the vibe") | Stay on seed, with two-hop |
|---|---|---|
| Card reads a pick | 1.0 | 1.0-1.7 on average, at most 2 |
| Time with an MP3 playing, typical | about 3 ms | 2.7-3.0 ms |
| Time at worst (each read behind the decoder's longest) | 14 ms (29 ms behind a FLAC) | 26 ms (55 ms behind a FLAC); about 33 ms through Arduino's `File` |
| CPU a pick | 1.4-1.9 ms (1.7-1.9 at the 0.55 floor) | 0.7-0.9 ms |

- **PSRAM:** 159-165 KB for all of AutoDJ: the u16 maps, the filters, 7
  cached slots (3.5 KB) and about 1 KB of hop rows.
- **The device budget's H1** (at most 2 card commands and 1 KB a pick) is
  met at its limit: 2 × 512 bytes.
- Appendix A's codes take about 4 reads a pick; the user relaxed H1 to
  that for AutoDJ's code path only (2026-10-13). The table path keeps 2.

### 11.7 What the default lists make (MEASURED-SYN)

200 seeds × 30 picks with mStream's web profile on the synthetic test card
(19,410 tracks), against mStream's own rule on the same vectors and against
the plain top-84 table (S84's head alone):

| At floors 0.55 / 0.65 / 0.75 | S84-55-169 | mStream | Plain top-84 |
|---|---|---|---|
| Rolling: pick cosine to the centroid | 0.708 / 0.765 / 0.807 | 0.648 / 0.719 / 0.797 | 0.804 / 0.804 / 0.813 |
| Rolling: genre-family switches | 66 / 53 / 43 % | 78 / 72 / 57 % | 43 / 43 / 41 % |
| Rolling: distinct tracks in 6,000 picks | 4,129 / 3,822 / 3,395 | 4,394 / 4,079 / 3,493 | 3,459 / 3,477 / 3,185 |
| Rolling: picks below the floor | 6.5 / 9.4 / 18.6 % | 0 / 0.1 / 0.4 % | 0.7 / 3.3 / 16.1 % |
| Seed: BPM/key breaks (the tables with two-hop) | 7 / 30 / 53 % | 3 / 18 / 44 % | 13 / 32 / 54 % |

- **The parity score MD** (0 = mStream's sessions) is 0.82 (replicates
  0.77-0.85), against 1.79 for the plain top-84 table: about half of the
  tight table's gap to mStream on average, most of it at the default 0.55.
- A plain top-169 table with two-hop breaks seed continuity less (about 6
  / 22-23 / 46 % under the same j rule), but doesn't wander (MD 1.34).
- **The seed-mode rates rest on uncalibrated BPM/key priors:** indicative
  only.
- **The long head's wander fades as the library grows:** MD 0.60 / 0.82 /
  1.04 at 10k / 19.4k / 50k tracks (the pick cosine at 0.65: 0.751 / 0.765
  / 0.786). Expect about 0.85-0.95 at the device's 23-27k ceiling. Seed
  mode stays stable. A writer can shorten the head later: the header's
  fields change, the format doesn't.
- These figures measure distance from mStream's rule, not listening
  quality.

### 11.8 Engine items (for the AutoDJ engine's PR)

- The join built in pieces (11.2).
- Slot reads from a raw FatFs `FIL` kept open, never through the card
  worker (11.3).
- The per-slot check, damaged against broken (8.5).
- The gate, with the floor clamped at selFloor (11.4); the dry fallback.
- Two-hop through the head in seed mode, with its j rule tuned (11.5).
- An optional "tight" setting that uses the head only.
- A seedable random stream, for tests and reproducible bug reports.
- `read.mpdj=2` in device.txt once the engine ships (7.4).
- ScanScheduler's DJ check job, made for v1's whole-file check, removed.
- A device bench: 200 slot reads from an open 10 MB file while an MP3 and
  a FLAC play, the same in two-hop pairs, and the slot check's time. It
  confirms 11.6.
- A synthetic transfer for the synthetic test card's tree (a tags file, a
  root and an MPDJ), so that the bench has a card.
- Backlog, not this format: a per-track BPM/key block in `library.idx`
  (about 80 KB at 20k) for tracks without a row, so that random with
  filters covers them too (M 7.1, U15).

---

## 12. Writer guidance (informative)

### 12.1 Fast and exact

The rule (5.2) is a sequential f64 sum. A matrix product in any order is
fast, but it isn't the rule. Two ways give the rule's levels:

- **An f64 matrix product with a guard band:** compute V = cos × 255 for
  every pair with an f64 GEMM, in any order; then recompute by the rule
  every pair with |V − (m + 0.5)| < 1e-9 for an integer m. For f32 vectors
  of norm about 1 and dim 1,280, any f64 summation order is within about
  7.3e-11 of the rule after × 255 (2 × dim × 2^−53 × 255 × |a| |b|): widen
  the band for other norms or dims. On the synthetic test card (19,410 ×
  1,280) `tools/mpdj.py`'s numpy path took 34 s for the lists (36 s for
  the whole build, its own check included), and no pair fell in the band
  (MEASURED-HOST).
- **The rule itself, in parallel:** one f64 accumulator a pair, the
  components in index order, the pairs spread over threads: 47.5 s for the
  whole card on 4 threads in plain C (MEASURED-HOST).

The pairs that decide a list are those at level Lf − 1 or more for kind 1
(55.3 M pairs on the card, about 2,850 a row) and at the k-th level − 1 or
more for kind 0 (2.27 M). Checking every pair covers both.

### 12.2 The counting sort

Levels are u8, so a row's candidates sort in linear time with 256 buckets:
count each level (the row itself and its song left out), then list the
candidates by level descending and, within a level, by row index
ascending. m and every rank of 5.4 follow from the counts. Kind 1 has no
top-k shortcut: every candidate at or above Lf counts towards m.

### 12.3 Not float32

A float32 dot product or GEMM is not the rule, and the strided selection
amplifies every difference: one level or one same-song difference at or
above Lf shifts m, and with it the row's whole tail. On the synthetic test
card a float32 writer gets 765-792 of the 19,410 rows (about 4 %) of
S84-55-169 wrong, 23 of them with a wrong m, against 52 for a top-169 table
(MEASURED-SYN). autodj-mid (13.5) holds such a pair on purpose.

### 12.4 Determinism checklist

Two writers agree byte for byte when they agree on: the row order (5.1);
the rule (5.2); the song keys (2.4, with Unicode 17's lowercase tables); the
primary file (5.3); the paths as the card stores them (M 2.8.5); the header
values that come from the commit (generation, cardId, builtTime,
tagsGeneration); the strings (4.1); selKind, k, H and Lf; the vectors, read
as 5.2 says. The fixtures (13) pin all of it. For nameKey they pin the
corners a per-character or simple lowercase gets wrong (autodj-tiny's
titles: Final_Sigma and U+0130's two code points, 13.5); the rest is the
Unicode 17.0 tables themselves, which the player checks against Rust's own
(`test/test_name_key`).

### 12.5 Licence

- Real files derive from EffNet-Discogs outputs, licensed CC BY-NC-SA 4.0:
  `license` and `attribution` carry it (M 2.13.4).
- No real table goes into firmware, a release, the fixtures, or a card
  made for others.
- The fixtures and the synthetic test card's index are synthetic, CC0.
  Their modelId is `synthetic-embedding`, so that they are never mistaken
  for real derived data.

---

## 13. Test vectors and fixtures

### 13.1 Where, and the licence

- In the player repo's `test/fixtures/card/`, under CC0 (its README; M
  2.17), with the card contract's other fixtures:
  - `libraries/<name>.json`: the descriptions, a writer's input (13.2);
  - `golden/<name>.bin`: what a writer MUST make of each, byte for byte;
  - `hardening/mpdj-*.bin`, listed in `hardening/index.json` (13.6);
  - the `"mpdj"` group of `vectors.json` (13.3).
- `tools/card_fixtures.py` makes them, with `tools/mpdj.py` (the reference
  writer and reader: standard library only, numpy an optional fast path)
  for every MPDJ part, and keeps them frozen: `python
  tools/card_fixtures.py --check` fails on any difference, and on any file
  it doesn't make. `python -m unittest discover -s tools -p "test_mpdj.py"`
  tests the reference itself.
- Every name, hash and vector in them is made up.

### 13.2 The description format

A description is the writer's whole input: every value a writer would
otherwise choose is given.

```
{
 "format": "MPDJ", "file": "<golden name>.bin",
 "major": 2, "minor": 0,                     optional, these defaults
 "generation": 41, "cardId": "0x5EEDC0DE0A1B2C3D", "builtTime": 1790000000,
 "tagsGeneration": 41,
 "k": 4, "selKind": 1, "selHead": 1, "selFloor": 100,
                                             selKind 0: selHead and selFloor left out
 "modelId": "synthetic-embedding", "modelVersion": "1", "metric": "cosine",
 "license": "CC0-1.0", "attribution": "synthetic test vectors",
 "dim": 8,
 "vectors": "<name>.f32",                    optional (below)
 "note": "...",
 "rows": [ {
   "hash": "<32 lowercase hex digits>",
   "files": [ {"path": "...", "artists": ["...", ...], "title": "...",
               "bpm10": 0, "camelot": 0, "bpmAnalysed": false} ],
   "embedding": [ ... ],
   "primaryPath": "...", "artistNameKey": "...", "songKey": "..." | null,
   "note": "..." } ]
}
```

- **cardId** is a hex string (integers above 2^53 don't survive every JSON
  reader). The other integers are JSON numbers. A selKind 0 description
  leaves selHead and selFloor out; the writer writes k and 0.
- **rows** may come in any order (the writer sorts them, 5.1). A row's
  files have distinct paths, and no path is in two rows.
- **path** is relative to `/music`, as the card stores it, with no leading
  slash: the path hash is over `/music/` followed by it.
- **artists** is the record's artist list (`[]` for none), and **title**
  the record's title field (a file object without `title` has no title
  field): exactly the values the MPTG record stores, so already through M
  2.3.6 (no control character, no empty value, no value over 255 bytes, no
  repeat in a list). A writer of this file applies nothing more to them:
  no cutting, no replacement, no deduplication; the trim and the folds are
  the keys' own (2.4). (The tags descriptions beside them differ: their tag
  values are raw, and each tags writer applies M 2.3.6.) **bpm10**,
  **camelot** and **bpmAnalysed** are the record's, optional, defaulting
  to 0, 0 and false. These are the MPTG record's values (5.3); no other
  field of a record matters to this file.
- **embedding** is dim numbers, each exactly an f32 value, written as the
  shortest decimal of that value as an f64: f32(0.6) is
  `0.6000000238418579`. A value that isn't an f32 is read as 5.2 says (the
  nearest f64, then the nearest f32, ties to even); one that isn't finite,
  or rounds past f32's range, is refused.
- **vectors,** when present, names a file beside the description that holds
  every row's dim values as raw little-endian f32, the rows in the
  description's order (not row order); the rows then have no `embedding`.
- **primaryPath, artistNameKey and songKey** are given for checking: a
  writer computes them (5.3; artistNameKey is the nameKey of the first
  non-blank artist value, "" when there is none; songKey is null when the
  row has none) and a test asserts that they agree.
- **note** fields are commentary.

### 13.3 The primitives: vectors.json "mpdj"

| Key | Shape | What it holds |
|---|---|---|
| `levels` | a list of `{cos, level}` (a `note` on some) | cosine → level: 5.2's table, the trap pair included |
| `floors` | `{who, cases}`, cases `{percent, level}` | slider hundredths → least level (11.4; who: informative) |
| `slotBytes` | a list of `{k, slotBytes}` | 1 → 0 (invalid), 2 → 256, 84 → 256, 85 → 512, 169 → 512, 170 → 0 |
| `headerBytes` | a list of `{sectionCount, slotBytes, minimum, headerBytes}` | 3.3's table (minimum: the header's size) |
| `stridedRanks` | a list of `{k, head, m}` with `ranks` (the whole list) or `pairs` (`{j, rank}`) | 5.4's rank cases; pairs for m = 2,258 |
| `slotCrc` | `{file, row, crc, unseeded}` | autodj-00000029's slot 3: CRC 0x790748C6; the same 252 bytes without the row's seed give 0x5BD60073 |
| `signature` | `{file, text, sig}` | autodj-00000029's signature text and selectionSig (6) |
| `vec` | `{file, dim, sha256}` | autodj-00000029's `vec` digest: dim 8, 038912838c44e3d8a2814ac75a927d6bf32122302c0ff5fca53f52f623a2370f |
| `deviceTxt` | `{who, cases}`, cases `{readMpdj, writesV2}`, `{noFile, writesV2}` or `{keyMissing, writesV2}` | 7.4's cases (who: software) |
| `trap` | `{file, rowA, rowB, level, levelF32, cos, cosF32}` | autodj-mid's float32 trap: rows 101 and 159, level 140 by the rule and 139 by a float32 sum, and both cosines |

`{who, cases}` is vectors.json's form for a group that binds one side (its
other groups use it too); `note` fields are commentary. Hex values are
strings: integers `0x`-prefixed, byte strings lowercase.

### 13.4 autodj-00000029: the hand-checkable file

The fixture card root's AutoDJ companion (7.2).

**The header values:** generation 41 (hence the name), cardId
0x5EEDC0DE0A1B2C3D, builtTime 1790000000, tagsGeneration 41; k 4,
selKind 1, H 1, Lf 100; the strings `synthetic-embedding`, `1`, `cosine`,
`CC0-1.0`, `synthetic test vectors`; dim 8.

**The rows.** Each embedding is 8 multiples of 1/16 whose squares sum to
exactly 1, so every cosine is a multiple of 1/256 and can be checked by
hand; the table gives them × 16. Each row has one card file and one record
unless said, and each artist value is quoted on its own.

| Row | Canonical hash | Embedding × 16 | Card path (under `/music`), path hash | Artist values | Title | bpm10, camelot, flags |
|---|---|---|---|---|---|---|
| 0 | 0123456789abcdef0123456789abcdef | 2 2 6 3 9 0 11 1 | `Generated Artist 01/Sample Album 01/01 - Sample Title 01.mp3`, 313F6910B7BBA50C | "Generated Artist 01" | "Sample Title 01" | 1210, 8 (8A), – |
| 1 | 1c3e5a7092b4d6f81c3e5a7092b4d6f8 | −2 12 3 2 9 −1 −2 −3 | `Generated Artist 02/Sample Album 02/01 - Sample Café.flac` twice: é as NFC (`C3 A9`), 6393C560DD800DEB, and as NFD (`65 CC 81`), ECD5A60887B28229 | none | "Sample Café" (both records) | the NFC file's record 0, 0, –; the NFD file's 1000, 0, – |
| 2 | 5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a | 6 0 3 1 7 6 10 −5 | `Generated Artist 04/Sample Album 04/03 - Sample Title 04.mp3`, 14538A0F0DBB9A85 | "Generated Artist 04", "Generated Artist 05", "generated␣␣artist 04" (two spaces) | "Sample Title 04" | 1280, 13 (1B), BPM_ANALYSED |
| 3 | 9e1f00000000000000000000000000a1 | 9 −1 5 9 −1 7 3 3 | `Generated Artist 03/Sample Album 03/01 - Sample Title 03.mp3`, 13193B14059675BE | "Generated Artist 03" | "Sample Title 03" | 954, 21 (9B), BPM_ANALYSED |
| 4 | b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0 | 7 1 12 4 0 −1 6 −3 | `Generated Artist 03/Sample Album 06/07 - SAMPLE TITLE 03.flac`, A58CEA0ABFA6EBE7 | "GENERATED ARTIST 03" | "SAMPLE TITLE 03" | 0, 0, – |
| 5 | fedcba9876543210fedcba9876543210 | 4 −3 9 2 0 4 11 3 | `Generated Artist 03/Sample Album 07/12 - Sample Title 03.opus`, 71C34B2CFA6396F3 | "Generated Artist 03" with a no-break space (U+00A0) between the first two words | " Sample Title 03" (a leading space) | 960, 21 (9B), – |

**The dot products × 256** (the cosine is the value / 256):

| | 0 | 1 | 2 | 3 | 4 | 5 |
|---|---|---|---|---|---|---|
| 0 | 256 | 100 | 201 | 100 | 163 | 186 |
| 1 | 100 | 256 | 51 | −28 | 40 | −48 |
| 2 | 201 | 51 | 256 | 128 | 151 | 172 |
| 3 | 100 | −28 | 128 | 256 | 160 | 172 |
| 4 | 163 | 40 | 151 | 160 | 256 | 194 |
| 5 | 186 | −48 | 172 | 172 | 194 | 256 |

**The keys:**

| Row | Primary file | nameKey of the first non-blank artist value | artistKey | Song key |
|---|---|---|---|---|
| 0 | its one file | `generated artist 01` | B460A770 | `generated artist 01\|sample title 01` |
| 1 | the NFC file (6393C560DD800DEB is the smaller), so DJRF's bpm10 is 0 | none | 00000000 | `\|sample café` |
| 2 | its one file | `generated artist 04` | B460AFEF | `generated artist 04, generated artist 05\|sample title 04` |
| 3 | its one file | `generated artist 03` | B460AAD6 | `generated artist 03\|sample title 03` |
| 4 | its one file | `generated artist 03` | B460AAD6 | `generated artist 03\|sample title 03` |
| 5 | its one file | `generated artist 03` | B460AAD6 | `generated artist 03\|sample title 03` |

Rows 3, 4 and 5 are one song (capitals, a no-break space, a leading
space): the same-song group [3, 4, 5].

**The lists** (row/level), with m counted at Lf = 100, and the slot CRCs:

| Row | m | List | What it shows | Slot CRC |
|---|---|---|---|---|
| 0 | 5 > k | 2/200, 5/185, 4/162, 3/100 | strided ranks 0, 1, 2, 4 with a stride remainder (m − 1 − H = 3, k − 1 − H = 2); the tie at Lf decided by the order (5.5) | E487C86F |
| 1 | 1 | 0/100, 2/51, 4/40, 3/0 | m < k: the top k, down below Lf; negative cosines (−28/256 and −48/256) at level 0, tied, the lower index kept | 993E690F |
| 2 | 4 = k | 0/200, 5/171, 4/150, 3/128 | m = k: the top k; a cosine of exactly 0.5 (127.5 gives 128) | 82736E17 |
| 3 | 2 | 2/128, 0/100, 1/0, unused | its own song (rows 4 and 5) left out: one entry unused | 790748C6 |
| 4 | 2 | 0/162, 2/150, 1/40, unused | the same | 9D3DDF4D |
| 5 | 2 | 0/185, 2/171, 1/0, unused | the same | 8ABB6A85 |

**The file:** 1,973 bytes, SHA-256
6a98332c933d01f511e96c85b4239c1adc0430d99d82583f8a5df8525d5410f2;
headerBytes 128, headerCrc 02ECB029, selectionSig
af4752829aa50afa2b5f2e6aecc56d59 (6).

| Section | Offset | Bytes | Count | Stride | CRC |
|---|---|---|---|---|---|
| DJSL | 256 | 1,536 | 6 | 256 | 8703FDFE |
| DJRF | 1,792 | 48 | 6 | 8 | D0ABF0FE |
| DJPH | 1,840 | 70 | 7 | 10 | 005F38AB |
| STRS | 1,912 | 61 | 0 | 0 | 310B3880 |

The two bytes 1,910-1,911 are the only gap. STRS holds the five strings at
offsets 1, 21, 23, 30 and 38.

**The bytes** (`hexdump -C` style; a `*` stands for lines equal to the one
before it; `python tools/mpdj.py dump <file> --hex` prints them):

```
00000000  4d 50 44 4a 02 00 00 00  80 00 00 00 04 00 00 00  |MPDJ............|
00000010  b5 07 00 00 29 00 00 00  3d 2c 1b 0a de c0 ed 5e  |....)...=,.....^|
00000020  29 b0 ec 02 00 00 00 00  06 00 00 00 07 00 00 00  |)...............|
00000030  04 00 00 01 02 01 01 01  80 3b b1 6a 29 00 00 00  |.........;.j)...|
00000040  af 47 52 82 9a a5 0a fa  2b 5f 2e 6a ec c5 6d 59  |.GR.....+_.j..mY|
00000050  01 00 00 00 15 00 00 00  17 00 00 00 1e 00 00 00  |................|
00000060  26 00 00 00 64 00 00 00  00 00 00 00 00 00 00 00  |&...d...........|
00000070  00 00 00 00 00 00 00 00  00 00 00 00 00 00 00 00  |................|
00000080  44 4a 53 4c 01 00 00 00  00 01 00 00 00 06 00 00  |DJSL............|
00000090  06 00 00 00 00 01 00 00  fe fd 03 87 00 00 00 00  |................|
000000a0  44 4a 52 46 01 00 00 00  00 07 00 00 30 00 00 00  |DJRF........0...|
000000b0  06 00 00 00 08 00 00 00  fe f0 ab d0 00 00 00 00  |................|
000000c0  44 4a 50 48 01 00 00 00  30 07 00 00 46 00 00 00  |DJPH....0...F...|
000000d0  07 00 00 00 0a 00 00 00  ab 38 5f 00 00 00 00 00  |.........8_.....|
000000e0  53 54 52 53 01 00 00 00  78 07 00 00 3d 00 00 00  |STRS....x...=...|
000000f0  00 00 00 00 00 00 00 00  80 38 0b 31 00 00 00 00  |.........8.1....|
00000100  02 00 05 00 04 00 03 00  c8 b9 a2 64 00 00 00 00  |...........d....|
00000110  00 00 00 00 00 00 00 00  00 00 00 00 00 00 00 00  |................|
*
000001f0  00 00 00 00 00 00 00 00  00 00 00 00 6f c8 87 e4  |............o...|
00000200  00 00 02 00 04 00 03 00  64 33 28 00 00 00 00 00  |........d3(.....|
00000210  00 00 00 00 00 00 00 00  00 00 00 00 00 00 00 00  |................|
*
000002f0  00 00 00 00 00 00 00 00  00 00 00 00 0f 69 3e 99  |.............i>.|
00000300  00 00 05 00 04 00 03 00  c8 ab 96 80 00 00 00 00  |................|
00000310  00 00 00 00 00 00 00 00  00 00 00 00 00 00 00 00  |................|
*
000003f0  00 00 00 00 00 00 00 00  00 00 00 00 17 6e 73 82  |.............ns.|
00000400  02 00 00 00 01 00 ff ff  80 64 00 00 00 00 00 00  |.........d......|
00000410  00 00 00 00 00 00 00 00  00 00 00 00 00 00 00 00  |................|
*
000004f0  00 00 00 00 00 00 00 00  00 00 00 00 c6 48 07 79  |.............H.y|
00000500  00 00 02 00 01 00 ff ff  a2 96 28 00 00 00 00 00  |..........(.....|
00000510  00 00 00 00 00 00 00 00  00 00 00 00 00 00 00 00  |................|
*
000005f0  00 00 00 00 00 00 00 00  00 00 00 00 4d df 3d 9d  |............M.=.|
00000600  00 00 02 00 01 00 ff ff  b9 ab 00 00 00 00 00 00  |................|
00000610  00 00 00 00 00 00 00 00  00 00 00 00 00 00 00 00  |................|
*
000006f0  00 00 00 00 00 00 00 00  00 00 00 00 85 6a bb 8a  |.............j..|
00000700  ba 04 08 00 70 a7 60 b4  00 00 00 00 00 00 00 00  |....p.`.........|
00000710  00 05 0d 01 ef af 60 b4  ba 03 15 01 d6 aa 60 b4  |......`.......`.|
00000720  00 00 00 00 d6 aa 60 b4  c0 03 15 00 d6 aa 60 b4  |......`.......`.|
00000730  be 75 96 05 14 3b 19 13  03 00 85 9a bb 0d 0f 8a  |.u...;..........|
00000740  53 14 02 00 0c a5 bb b7  10 69 3f 31 00 00 eb 0d  |S........i?1....|
00000750  80 dd 60 c5 93 63 01 00  f3 96 63 fa 2c 4b c3 71  |..`..c....c.,K.q|
00000760  05 00 e7 eb a6 bf 0a ea  8c a5 04 00 29 82 b2 87  |............)...|
00000770  08 a6 d5 ec 01 00 00 00  00 73 79 6e 74 68 65 74  |.........synthet|
00000780  69 63 2d 65 6d 62 65 64  64 69 6e 67 00 31 00 63  |ic-embedding.1.c|
00000790  6f 73 69 6e 65 00 43 43  30 2d 31 2e 30 00 73 79  |osine.CC0-1.0.sy|
000007a0  6e 74 68 65 74 69 63 20  74 65 73 74 20 76 65 63  |nthetic test vec|
000007b0  74 6f 72 73 00                                    |tors.|
000007b5
```

**Reading it by hand:**

- **0x00-0x27, the common header:** `MPDJ`; major 2, minor 0; headerBytes
  0x80 = 128; 4 sections; fileBytes 0x7B5 = 1,973; generation 0x29 = 41;
  cardId; headerCrc `29 B0 EC 02` = 0x02ECB029; reserved.
- **0x28-0x67, MPDJ's header:** rowCount 6, pathCount 7; k 4; slotBytes
  0x100 = 256; indexBytes 2, scoreKind 1, selKind 1, selHead 1; builtTime
  0x6AB13B80 = 1,790,000,000; tagsGeneration 41; selectionSig at 0x40; the
  five string offsets 1, 0x15, 0x17, 0x1E, 0x26; selFloor 0x64 = 100; then
  zeros to 0x80.
- **0x80-0xFF, the directory:** two lines a section. DJSL's entry reads
  type `DJSL`, flags 1 (REQUIRED), offset 0x100, bytes 0x600, count 6,
  stride 0x100, crc 0x8703FDFE.
- **Slot 0, at 0x100:** the indexes `02 00 05 00 04 00 03 00` = 2, 5, 4, 3;
  the levels `C8 B9 A2 64` = 200, 185, 162, 100; zeros; at 0x1FC the CRC
  `6F C8 87 E4` = 0xE487C86F, the CRC-32 of `00 00 00 00` followed by the
  slot's first 252 bytes.
- **Slot 3, at 0x400:** the indexes 2, 0, 1 and `FF FF` (unused); the
  levels 128, 100, 0, and the unused entry's 0.
- **DJRF, at 0x700:** row 0 is `BA 04 08 00 70 A7 60 B4`: bpm10 1210,
  camelot 8, flags 0, artistKey 0xB460A770. Row 2 is `00 05 0D 01 EF AF 60
  B4`: bpm10 1280, camelot 13, BPM_ANALYSED, artistKey 0xB460AFEF.
- **DJPH, at 0x730:** the first entry is `BE 75 96 05 14 3B 19 13 03 00`:
  path hash 0x13193B14059675BE, row 3.
- **STRS, at 0x778:** a NUL, then the five strings, each with its NUL.

### 13.5 The other goldens

| File | Bytes | SHA-256 | headerCrc | selectionSig |
|---|---|---|---|---|
| autodj-00000029 | 1,973 | 6a98332c933d01f511e96c85b4239c1adc0430d99d82583f8a5df8525d5410f2 | 02ECB029 | af4752829aa50afa2b5f2e6aecc56d59 |
| autodj-small-top | 1,973 | 0c680d20cfc6c9c1d62a56ca32f87313301188f7deb650afba43910c624a7c8c | 307F4F2F | efc89507bd5ed660cd117229eb732b64 |
| autodj-small-default | 3,765 | 3656647e65e7e4f43c3047bd6322aeb7c1b159c17c067877c7d473793a783b82 | E5D49101 | ba7867a11384a3e89d8bef01dd8cea64 |
| autodj-tiny | 877 | 63406a8f17580fd109ed7a0063ddbb3868499087ac18b8c0da6891ec6319e1c8 | 0465883E | eee1573c461b87588869390381167d39 |
| autodj-pair | 869 | 6f34bfe3b16dcb34ed66933d974ca54ad30b3a45767a82836f6d585bdd8017bd | 02FECBBE | c233adee0a6af85e491f573c36f138b7 |
| autodj-mid | 55,133 | a387f0452d1f04f263d57bf57ef623741ed68144179d5512ee1c6618f249dbae | FFC43E94 | abcb2bac4a1f87ac7c995e6faee149e5 |
| autodj-mid-default | 106,589 | 19ee25578ed03b2ddcdf4b4db118444b68bf80c9cb07906a25e77a3d67dce9c0 | 28C755F4 | dc8da02d708c02ce8ec7194ef1042639 |

- **autodj-small-top:** 13.4's rows as a plain top-k table (selKind 0,
  selHead 4, selFloor 0). Row 0 is 2/200, 5/185, 4/162, 1/100 (slot CRC
  E0027F23); the other rows are 13.4's.
- **autodj-small-default:** 13.4's rows at the writer's defaults (k 169,
  selKind 1, H 84, Lf 140): 512-byte slots, headerBytes 384, the padding
  byte at 507, DJSL at 512. Every row has m ≤ k, so every list is its row's
  whole candidate order, down to level 0: row 0 is 2/200, 5/185, 4/162,
  1/100, 3/100; row 1 0/100, 2/51, 4/40, 3/0, 5/0; row 2 0/200, 5/171,
  4/150, 3/128, 1/51; rows 3-5 as 13.4.
- **autodj-tiny:** two rows of one song, so every list is empty (c = 0).
  Row cc00…00 (`cc` and 30 zeros) has one file,
  `Generated Artist 31/01 - Sample Title 31.mp3`, with "Generated Artist
  31" and the title "Sample Ti̇tle 31 ΤΙΤΛΟΣ" (`i` then U+0307, the
  combining dot above; Greek capitals), and the embedding f32(0.6),
  f32(0.8), written `0.6000000238418579` and `0.800000011920929`. Row
  dd00…00 has two files, `Generated Artist 31/Live/01 - Sample Title
  31.mp3` and `Generated Artist 31/Copy/01 - Sample Title 31.mp3`, each
  with "generated artist 31" and the title "SAMPLE TİTLE 31 τιτλος"
  (U+0130, and the final sigma ς), and the embedding 1, 0. k 2, selKind 0,
  dim 2, generation 7; 3 DJPH entries.
  - The two titles are one song only under 2.4's lowercase: U+0130's full
    lowercase is `i` and U+0307, and Final_Sigma makes the closing Σ a ς.
    Both keys are `generated artist 31|sample ti̇tle 31 τιτλος`. A
    per-character or simple lowercase (`towlower`: U+0130 → `i`, Σ → σ)
    makes two songs of them, and so two lists that aren't empty.
- **autodj-pair:** two title-less rows of one artist, with the embeddings
  1, 0 and −1, 0. Neither has a song key, so each lists the other, at level
  0 (c = 1). Row aa00…00's title is three spaces (kept by M 2.3.6, blank
  here); row bb00…00's record has no title field. A title from the file
  names would have made them one song. k 3, selKind 0, dim 2, generation 7.
- **autodj-mid and autodj-mid-default:** 200 generated rows, dim 16,
  generation 9, at k 24, H 8, Lf 140 (256-byte slots) and at the writer's
  defaults (512-byte slots). Below.

**autodj-mid** comes from `tools/mpdj.py`'s `tv_mid()`, which uses the
standard library only, so its bytes are the same on any Python 3:

- `random.Random(20261013)`; integer components from `getrandbits`; each
  vector normalised in f64 (an exact sum of squares, a correctly rounded
  square root and division) and rounded to f32. A main cone, where most
  pairs are at cosine 0.55 or more, and a group of 14 outliers with low m.
  The names are "Generated Artist NN", "Sample Album NN" and "Sample Title
  NNN".
- Its vectors are in `libraries/autodj-mid.f32` (12,800 bytes, SHA-256
  60c4c875b4f098713cc900d722c7b5684a06139449e60aa7e91594768ef9a5db), which
  both descriptions name.
- It holds, in both builds, and the generator asserts:
  - (a) rows with m > k, among them rows with (m − 1 − H) not a multiple
    of (k − 1 − H);
  - (b) rows with m ≤ k whose lists reach below Lf;
  - (c) rows whose last entry a tie at Lf decides;
  - (d) **the float32 trap:** rows 101 and 159 are at level 140 = Lf by the
    rule and 139 by a float32 sum, row 159 at rank 184 among row 101's
    candidates, so a float32 writer gets the slots of rows 101 and 159
    wrong, and only those;
  - (e) a title-less row (130) beside a row of the same artist (80) whose
    title is the one row 130's file name carries: each lists the other;
  - (f) a same-song group of three (rows 12, 15 and 72: capitals, extra
    spaces), none listing another;
  - (g) one recording at two paths (row 49, NFC and NFD): 201 DJPH entries
    for 200 rows;
  - (h) rows with no artist (52, 118, 134, 141, 143, 144), one with three
    values (111), and a record whose camelot is 25, written as 0.
- Each description's note lists the rows of (a)-(c) for its own build.

### 13.6 Hardening files

Each file breaks one rule of section 8 and keeps everything else valid:
every section CRC and the header CRC are made right; a slot edited on
purpose has its CRC made right unless the CRC is the fault; and when a file
adds, drops or reorders sections, headerBytes follows the alignment rule
unless the alignment is the fault. `hardening/index.json` lists each with:

- **uses:** `all` (every section, every slot) or `device` (DJRF, DJPH and
  STRS; slots one by one as read);
- **expect:** absent or present;
- **why:** the reference reader's reason (8.1-8.4, in 8.6's order);
- **same:** for a present file, the golden it reads the same as: every
  row's filter record (its 8 known bytes, as read) and used slot entries
  (the c entries of 4.3, not the unused entries' bytes), every DJPH hash's
  row, the five strings;
- **slot:** when given, how one slot reads when the device reads it alone
  (8.5): damaged or broken.

A reader MUST reach the verdict (absent or present); the reasons are there
for a reader that follows 8.6's order and wants to compare. From
autodj-00000029 unless said:

| File | What is broken | Reason | Slot |
|---|---|---|---|
| mpdj-major-1 | major 1 (the retired v1 draft: an older major) | major | |
| mpdj-major-3 | major 3 (a newer major) | major | |
| mpdj-header-bytes-short | headerBytes 96, below 104 | headerBytes | |
| mpdj-misaligned | headerBytes 136: DJSL at 264, off a slot boundary | djLayout | |
| mpdj-djsl-not-first | DJRF before DJSL | djLayout | |
| mpdj-section-order | STRS before DJPH | djLayout | |
| mpdj-rows-zero | rowCount 0 | enum | |
| mpdj-k-one | k 1 | enum | |
| mpdj-k-170 | k 170 | enum | |
| mpdj-slot-bytes | slotBytes 512 with k 4 | enum | |
| mpdj-index-bytes | indexBytes 4 | enum | |
| mpdj-score-kind | scoreKind 2 | enum | |
| mpdj-sel-kind0-head | selKind 0 with selHead 3, not k | djSelection | |
| mpdj-sel-kind0-floor | selKind 0 with selFloor 140, not 0 | djSelection | |
| mpdj-sel-head-zero | selKind 1 with selHead 0 | djSelection | |
| mpdj-sel-head-big | selKind 1 with selHead 3 = k − 1 | djSelection | |
| mpdj-sel-floor-zero | selKind 1 with selFloor 0 | djSelection | |
| mpdj-sel-unknown-head | an unknown selKind 7 with selHead 5, above k | djSelection | |
| mpdj-row-count | rowCount 7; DJSL and DJRF hold 6 | counts | |
| mpdj-path-count | pathCount 8; DJPH holds 7 | counts | |
| mpdj-djrf-count | DJRF of 5 records for 6 rows | counts | |
| mpdj-slot-stride | DJSL's stride 512 with slotBytes 256 | shape | |
| mpdj-missing-djrf | no DJRF | missing | |
| mpdj-unknown-required | an unknown REQUIRED section, DJZZ, before STRS | unknownRequired | |
| mpdj-paths-empty-stride-zero | pathCount 0 and an empty DJPH with stride 0 (an empty array keeps its stride, 4.5) | shape | |
| mpdj-header-bytes-wrap | headerBytes 0xFFFFFFF8: the directory's end is past the file (wrapped to 32 bits it is 120, and the header CRC is right for that) | short | |
| mpdj-section-end-wrap | an optional section of 0xFFFFFFF8 bytes before STRS, its end past the file (wrapped to 32 bits it is where this file's STRS starts: such a reader finds the file present) | layout | |
| mpdj-array-bytes-wrap | DJPH's count and pathCount 7 + 2^31: count × stride isn't its 70 bytes (wrapped to 32 bits it is) | shape | |
| mpdj-strs-first-byte | STRS's byte 0 isn't NUL | string | |
| mpdj-string-no-nul | the last string has no NUL | string | |
| mpdj-string-out-of-range | attribution's offset at STRS's end | string | |
| mpdj-string-order | modelId's and modelVersion's offsets swapped (each string valid) | stringOrder | |
| mpdj-paths-unsorted | two DJPH entries swapped | djPaths | |
| mpdj-paths-equal | two equal path hashes | djPaths | |
| mpdj-paths-row-range | a DJPH row of 6 (rowCount 6) | djPaths | |
| mpdj-slot-crc | slot 2's CRC wrong | djSlotCrc | row 2 damaged |
| mpdj-slot-wrong-row | slots 1 and 2 swapped, each CRC right for its own row | djSlotCrc | row 1 damaged |
| mpdj-slot-unused | slot 3: the unused entry moved before row 1's | djSlotUnused | row 3 broken |
| mpdj-slot-index-range | slot 0: entry 3's index 6 = rowCount | djSlotIndex | row 0 broken |
| mpdj-slot-self | slot 0: entry 3's index 0, the row itself | djSlotIndex | row 0 broken |
| mpdj-slot-level-rises | slot 0: entry 1's level 201 after entry 0's 200 | djSlotOrder | row 0 broken |
| mpdj-slot-tie-order | slot 2: entry 2's level 171, so rows 5 and 4 tie with the index falling | djSlotOrder | row 2 broken |

Present, each reading the same as autodj-00000029 unless said:

| File | What it holds | Slot |
|---|---|---|
| mpdj-minor-1 | minor 1: a 112-byte header (a field at 104, a string offset at 108 pointing after attribution's string), DJRF and DJPH strides of 12 with non-zero tails, and an unknown optional section DJZZ before STRS; headerBytes 352 | |
| mpdj-unknown-optional | minor 0 with an unknown optional section DJZZ before STRS | |
| mpdj-header-bigger | headerBytes 384 with 256-byte slots: not the smallest, still aligned | |
| mpdj-paths-empty | pathCount 0 (every card file's hash in a collision): DJPH empty, with count 0 and stride 10; the rows and their lists unchanged. Not the same as autodj-00000029: no path finds a row | |
| mpdj-sel-unknown-kind | an unknown selKind 7 with selHead 2 and selFloor 9 | |
| mpdj-slot-unused-level | slot 3: the unused entry's level byte 7 (the CRC made right) | |
| mpdj-slot-padding | autodj-small-default's slot 0 with its padding byte 507 set (the CRC made right); the same as autodj-small-default | |
| mpdj-uses-device-slot-crc | mpdj-slot-crc's fault read with the uses `device`: the join's sections are good, and the slot fails only when read | row 2 damaged |

### 13.7 How they were checked (2026-10-13)

- `tools/mpdj.py` builds every golden from its description by the rule as
  written, with no shortcut, and checks the lists against 13.4's typed
  lists and against a second, plainer reading of the rule
  (`tools/test_mpdj.py`). It reads every hardening file to index.json's
  verdict, reason and slot read.
- No fixture holds a real 64-bit path-hash collision (none can be made
  from plain paths here): the tool's tests and the player's inject one, a
  hash two card files share, and check that both are left out of DJPH
  while their row stays (4.5).
- **Two independent implementations,** sharing no code with
  `tools/mpdj.py` or `lib/core`: one in C, written from the format's
  normative text as it was planned, and one in Python, written from this
  document alone (with the M sections it cites). Each rebuilt all seven
  goldens byte for byte from their descriptions (the Python one computing
  every key itself); read all 50 MPDJ hardening files to the same reasons
  and slot reads; and read every truncation and every single-bit flip of
  the five small goldens as this document says: every truncation absent,
  every flip absent except in the gap bytes, which no CRC covers (3.2) and
  which read the same. A writer whose lowercase works one character at a
  time fails autodj-tiny.
- **At scale:** the synthetic test card's index (19,410 rows of 1,280-dim
  vectors, S84-55-169) was built with `tools/mpdj.py`'s numpy path in 36 s
  (34 s of it the lists; no pair in the guard band), rebuilt byte for byte
  by the C writer by
  the rule as written in 47.5 s on 4 threads, and read as present by both
  (MEASURED-HOST). Its lists equal the research's on every row. It stays
  out of the repo: its vectors are 99 MB, and it conforms only beside a
  tags file and a root, which the synthetic card doesn't have yet (11.8's
  synthetic transfer).
- The player's own reader and writer (`lib/core/CardAutoDj`) are tested
  against the same fixtures in `test/test_card_files`,
  `test/test_card_contract` and `test/test_card_autodj`.

---

## Appendix A. Planned minor 1: cell-ordered codes

**Status:** informative until minor 1 is published; its layout is the plan
of record (the user's D2, 2026-10-13). The parts marked **provisional**
depend on the real vectors' spectrum, and wait for one aggregate-only run
on the server's real vectors (approved by the user, not yet run). Nothing
in minor 0 depends on any of it.

### A.1 What it buys and what it costs

- **What:** per-row PCA codes stored on the card in cells, with a small
  cell directory resident. A rolling pick reads about 4 sampled cells and
  draws with 1/π weights, which reproduces mStream's own rolling DJ
  (MEASURED-SYN, c16: parity score MD 0.21; the pick cosine within +0.001 to
  +0.007 of mStream's; 5-12 % of picks below the floor, or 2.4-4.5 % with
  c32).
- **Seed mode, a session's first pick and "more like this"** stay the
  table's: codes alone fail on a lone anchor.
- **Cost at 20k** (ESTIMATED; c16, C = 128 cells, 4 read a pick):
  - +0.28 MB of file, 10.88 MB in all with the default table;
  - about 28 KB more resident: all of AutoDJ at about 187-193 KB with
    the default table (159-165 KB, 11.6);
  - 1.0-1.4 ms of CPU in one pass;
  - 3.9 card commands a pick on average (p90 5, at most 9 at pick 2, which
    looks up two anchors through DJCP; keeping DJCP resident, 40 KB, removes
    those), 6 KB a pick;
  - 14 ms typical with an MP3 playing, about 0.25 s at worst (FLAC).
- **It breaks only H1** (2 commands a pick), which the user relaxed to
  about 4 reads for this path: at most 16 KB a call, never in the card
  worker, the pick computed a track ahead.
- **Not the plan:** round 1's resident codes. They sweep 160 KB of PSRAM a
  pick, which breaks H7 (no sweep over about 100 KB a pick), and hold
  AutoDJ at 320-325 KB.

### A.2 Pinned now, and provisional

**Pinned** (independent of the vectors' spectrum): the four sections, their
order, flags and layouts (A.4); the entry and cell layout; the CRC's seed;
the integer centroid and spread; the KDA and KDF median trees; the
projection and quantisation arithmetic; the reader checks (A.6); the
rewrite rule; codesSig's text; and the principle that the device never
needs the basis.

**Provisional:**

- d (16 or 32) and bits (4 or 8): c16 (PCA-16, 4 bits, 8-byte codes)
  against c32 (PCA-32, 8 bits, 32-byte codes);
- the quantiser's clip order statistics (0.1 % and 99.9 % today);
- the calibration's form and its fitting procedure (round 1's form (c): a
  pair affine plus per-n affines for n = 1-5, least squares on simulated
  walks);
- about 150 rows a cell (A.5's rule for C);
- the cells read a pick (4) and the sampling model (A.7);
- whether a frozen basis (basisKind 2) is wanted.

### A.3 The basis

- **The writer chooses it; the device never reads it.** The device reads
  the codes, each dimension's (lo, step) and the calibration's constants.
- **basisId** is the first 16 bytes of the SHA-256 of the basis as stored:
  d vectors of dim f32 values, little-endian, vector by vector.
- **basisKind 1, fitted to the file's own rows,** by this procedure
  (SHOULD):
  1. M = the sum over the rows, in row order, of x xᵀ in f64 (x is the
     row's f32 components as f64);
  2. M's d eigenvectors of largest eigenvalue, by eigenvalue descending,
     each of unit length;
  3. the sign rule: each vector's component of largest absolute value is
     positive (on a tie, the lowest index's);
  4. each value rounded to f32. That f32 matrix is the basis.

  Two writers agree unless an f64 value sits within its last bits of an
  f32 rounding boundary. If they disagree, only the code sections differ:
  codesSig names the basis (A.5), and the table is untouched.
- **basisKind 2, a frozen published basis.** It needs real vectors and a
  licence decision (the fixtures' licence question, M 7.1, U18): later,
  the user's.

### A.4 The sections

All four or none, REQUIRED clear, between DJPH and STRS, in this order. A
reader that finds some but not all, or any failing its checks (A.6),
treats the **code sections** as absent and keeps the table.

**DJCQ** (blob, 96 + 8d bytes): the code's parameters.

| Off | Size | Field |
|---|---|---|
| 0 | 1 | codeKind: 1 = a PCA projection, a uniform scalar quantiser a dimension, and form-(c) calibration |
| 1 | 1 | d, the dimensions |
| 2 | 1 | bits a dimension: 4 or 8 |
| 3 | 1 | the partition rule: 1 KDA, 2 KDF |
| 4 | 4 | C, the cells: a power of two |
| 8 | 2 | cellAlign: 512 |
| 10 | 2 | codeBytes = ⌈d × bits / 8⌉ |
| 12 | 1 | basisKind: 1 fitted, 2 frozen |
| 13 | 3 | reserved, 0 |
| 16 | 16 | basisId (A.3) |
| 32 | 16 | codesSig (A.5) |
| 48 | 8d | for each dimension i: lo_i (f32), step_i (f32) |
| 48 + 8d | 48 | the calibration: 12 f32 (the pair affine a, b; then a_n, b_n for n = 1 to 5) |

**DJCD** (array, count C, stride 12 + 3d): the cell directory.

| Off | Size | Field |
|---|---|---|
| 0 | 4 | offset: the file offset of the cell's first entry, a multiple of 512 |
| 4 | 2 | count: the cell's entries |
| 6 | 2 | reserved, 0 |
| 8 | 4 | crc: CRC-32 over the cell's index as a u32 (little-endian), then its count × (2 + codeBytes) entry bytes |
| 12 | 2d | the centroid, a u16 a dimension: ⌊(512 × Σq + count) / (2 × count)⌋, that is round(256 × the mean code), half up; 0 for an empty cell |
| 12 + 2d | d | the spread, a u8 a dimension: min(255, isqrt(⌊V × 4^(8 − bits) / count²⌋)), with V = count × Σq² − (Σq)² (about 16 × the standard deviation for 4-bit codes) |

The sums are over the cell's codes q in that dimension, in exact integers;
isqrt is the integer square root (the largest integer whose square is at
most the value).

**DJCC** (blob): the entries, in cell order. An entry is a u16 row
(little-endian), then codeBytes of code: with 4 bits, dimensions 2i and
2i + 1 in byte i's low and high nibble; with 8 bits, one byte a dimension.
Within a cell the rows ascend. Cell 0 starts at the first multiple of 512 at
or after DJCC's offset, and each next cell at the first multiple of 512 at
or after the end of the one before; the bytes between are zero. The section
ends at the last cell's last entry.

**DJCP** (array, count rowCount, stride 2): DJCP[r] is row r's entry number
in DJCC, the entries numbered from 0 across the cells in cell order.

### A.5 Writer rules

- **Every row has exactly one entry.**
- **The projection:** y_i is the sum over the dim components t of
  basis_i[t] × x[t], each product and partial sum in f64, in index order:
  the cosine's arithmetic (5.2).
- **The quantiser** (provisional statistics), for each dimension i over the
  n rows: lo = the k_lo-th smallest y_i (counting from 0), hi = the k_hi-th,
  with k_lo = ⌊0.001 × (n − 1)⌋ and k_hi = n − 1 − k_lo; step = f32((hi −
  lo) / (2^bits − 1)), or f32(1) when hi = lo; lo is stored as f32. Then
  code = clamp(round((y − lo) / step), 0, 2^bits − 1), computed in f64 from
  the stored f32 lo and step, rounded half away from zero.
- **The cells** (provisional: about 150 rows a cell): C is the largest
  power of two with C × 150 ≤ rowCount, and 1 below 300 rows. That is 128 at
  20k and on the 19,410-row test card, and 256 at 50k.
- **The partition** is a median tree of depth log2(C) over the integer
  codes. At each node its m rows are sorted by (their code in the split
  dimension, row index), and the first ⌊m / 2⌋ go to the left child, the
  rest to the right. The leaves, numbered left to right, are the cell
  order.
  - **KDF** splits every node at depth j (from 0) on dimension j + 1, the
    dimensions numbered from 0; dimension 0, the basis's first, is never
    split on. It needs log2(C) < d.
  - **KDA** splits each node on its own widest dimension: the largest
    step_i² × (m × Σq² − (Σq)²) over the node's rows, step_i² computed
    exactly in f64, the integer converted to f64, and one f64 multiply; on
    a tie, the lowest dimension.
  - Never k-means: it isn't reproducible (with another seed, 53-67 % of
    rows stay in the same cell).
- **The calibration** is fitted by the writer (provisional procedure) and
  stored as f32.
- **codesSig** is the first 16 bytes of the SHA-256 of this text, every
  line ended by one LF:

  ```
  MPDJ-CODES 1
  codes <codeKind> <d> <bits>
  basis <basisKind> <basisId as 32 lowercase hex>
  cells <rule> <C> 512
  vec <dim> <64 lowercase hex>     as the selection signature's (6)
  <canonical hash of row 0>        one line a row, in row order
  ...
  ```

- **The selection signature isn't extended** by the codes: it stays what
  DJSL depends on. A different basis or quantiser then rewrites only the
  code sections (0.28 MB at 20k), never the table's minute of compute, and
  the root's COMP key and the device's check stay as they are.
- **Rewriting** (7.3 gains a row): the same selection signature, but DJRF,
  DJPH, STRS or the code sections would change: write a new file, which MAY
  copy DJSL. A writer carries code sections forward only when its own
  codesSig equals the file's.

### A.6 Reader checks

- codeKind, d, bits and the rule are values the reader knows; codeBytes =
  ⌈d × bits / 8⌉; cellAlign = 512; DJCQ's length is 96 + 8d.
- C is a power of two; DJCD's count is C and its stride 12 + 3d; DJCP's
  count is rowCount and its stride 2.
- The cell offsets ascend and are multiples of 512, each cell (count × (2
  + codeBytes) bytes) lies inside DJCC, and the counts sum to rowCount.
- On each cell read: its CRC, every row below rowCount, the rows
  ascending.
- At each DJCP lookup: the entry holds row r.

Any failure makes the code sections absent; the table still works.

### A.7 The device's code path (informative)

- A rolling pick with codes:
  1. weigh each cell's expected matches above the floor from its centroid
     and spread (a Gaussian model);
  2. draw n = 4 cells by systematic sampling, with probabilities π_c
     proportional to the weights (capped at 1, summing to n), the 2 nearest
     cells always taken;
  3. read them (at most 16 KB a call), gate their rows on the calibrated
     code estimate, filter, and draw from the best tier with the weight
     1/π_c of the row's cell: the draw is then uniform over the whole gated
     pool, in expectation.
- Provisional: n, the forced cells and the sampling model.
- Seed mode, the first pick and "more like this" use the table (11).
- The research measured this with a top-100 table serving lone anchors;
  with the default table, seed mode is the default table's.

---

## Appendix B. Changes from the MPDJ v1 draft

v1 (docs/METADATA.md 2.13, from 2026-10-07) never had a reader. Released
firmware 0.8.0-0.10.0 advertised `read.mpdj=1` without one (7.4). A v2
draft between them (the research's round 1) coded levels as 4-bit steps
with escape bytes; this document keeps plain bytes instead.

| v1 draft | v2 | Why |
|---|---|---|
| major 1 | major 2 | every section changed; a v1 file is absent to a v2 reader |
| DJRW, 24 bytes a row: the primary path hash, a hash prefix, bpm10, camelot, flags, artistKey | DJRF, 8 bytes a row: bpm10, camelot, flags, artistKey | the device never needed the other 16 bytes: the join reads 0.36 MB at 20k, not 0.72 |
| DJNB, k × 3 bytes a row packed after DJRW (300-byte rows at k = 100: 1.58 sectors a read on average) | DJSL first, fixed 256- or 512-byte slots on slot boundaries, the header padded (3.3) | one single-sector read a pick |
| one CRC over all of DJNB: a 6 MB check after every commit before AutoDJ may use the file | a CRC a slot, seeded with the row | no whole-file pass; a pick checks what it reads; a misplaced read is caught |
| the top K | a selection in the header: top-k, or strided with an exact head | the strided tail makes the DJ wander most of the way to mStream's at the default floor |
| indexBytes 2, or 4 above 65,535 rows | 2 only; at most 65,535 rows | no device holds more; an untestable branch gone |
| DJPH, 12 bytes (a u32 row), sorted by (hash, row) | 10 bytes (a u16 row), sorted by hash, each hash once; collisions left out | smaller, and never ambiguous |
| signature `MPDJ-SEL 1`: the rows, model, metric, k | `MPDJ-SEL 2`: and the `sel`, `vec` and `same` lines | equal signatures mean equal slots, whatever re-analysis or retag happened |
| the song key's title fell back to the file name | from the primary file's MPTG record only; a blank title gives none | no portable rule for file-name titles exists |
| artistKey from the first artist value | from the first non-blank one | a blank first value gave the key 0 |
| a new file when DJPH changes | a new file when DJRF, DJPH or STRS change; the slots may be copied | DJRF holds tag data |
| readers didn't check the lists | readers check each slot they use (8.4) | a writer's bug makes the file absent, rather than changing picks silently |
| no word on device.txt | v2 only when `read.mpdj` lists 2 (7.4) | today's firmware advertises v1 without reading it |

**The real gains over v1** (ESTIMATED): no whole-file pass (v1's: 3.5-5 s
idle, about 10 s with an MP3 playing, after every sync), half the join, and
a misplaced read caught. The worst-case pick halves (14 ms against 26 ms
with an MP3 playing) only in rolling mode: seed mode with two-hop reads two
slots on many picks, and so keeps v1's two-command tail. The default file is
58 % larger than v1's at K = 100 (10.60 MB against 6.72 MB at 20k), by
choice: the space buys the long head and the wander.

---

## Appendix C. Where the numbers come from

- **The AutoDJ index research** (2026-10-12/13, in the session's
  scratchpad, not in the repo): round 1's report and spec draft, the
  critique, round 2's studies (the strided selection, two-hop, the
  cell-ordered codes), round 2's report, and its two checks (a
  reproduction and a completeness review). Every figure quoted here was
  compared with its source and with the checks' corrections for this
  document; every byte figure of sections 3, 4, 6, 10 and 13 comes from the
  fixtures and `tools/mpdj.py` (`dump --hex`, `signature`, `size`).
- **The labels:**
  - **MEASURED-SYN:** measured on the synthetic test card (19,410 made-up
    tracks whose 1,280-dim vectors mimic a real library's aggregates and
    its neighbour scores by rank), or its 10k and 50k siblings, through one
    simulator of mStream's DJ rules: 200 seeds × 30 picks, mStream's web
    profile, floors 0.55 / 0.65 / 0.75, the same seeds and draws for every
    candidate.
  - **MEASURED-HOST:** timed on a PC.
  - **MEASURED:** on the Core2 (11.3's read-path fit, docs/OPUS.md 10.1).
  - **READ:** read in mStream's source by the research (round 1): the
    model registry's id, version, licence and attribution (4.1), and the
    similarity's summation (5.2).
  - **ESTIMATED:** device time and memory, worked out from the Core2's
    measured card and CPU rates.
- **The device budget's limits** quoted here: **H1** at most 2 card
  commands and 1 KB a pick, from a file kept open; **H5** about 256 KB for
  the largest single allocation; **H7** about 8 ms of CPU a loop pass, and
  no PSRAM sweep over about 100 KB a pick; **H9** no pick read in the card
  worker's queue; **H14** the file about 10 MB at 20k (soft).
- **What this can't tell:** listening quality (every session figure is a
  distance from mStream's rule, which is not ground truth); the real
  library's spectrum, hubness and near-duplicate cliffs (the synthetic
  vectors match its aggregates only); device timing (every millisecond is
  an estimate until 11.8's bench runs). The long head was measured on the
  test card with one replicate; on the 10k and 50k sets, only without
  two-hop.
