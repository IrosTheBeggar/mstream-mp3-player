# Waveform index: `waves-<gen>.bin` (MPWF v1)

**Status (2026-11-01):** the format of record for the waveform index that
the transfer software writes to the card, mstream-terminal first. The
player's reader is `lib/core/CardWaves` (host-tested), and Now Playing draws
what it reads. The reference writer and reader is `tools/mpwf.py`. Every test
vector (13) is synthetic.

Reading a file needs nothing but this document, in any language. Writing one
also follows a few rules of the card contract, docs/METADATA.md ("M" below);
where this document relies on one, it restates it and cites the section. "A"
is docs/AUTODJ-INDEX.md, whose slot pattern this format shares.

**Why these numbers:** 240 bars outlast the 99 columns the player draws
today. A 256-byte slot is one small read, half a sector, and 5 MB at 20,000
tracks. The bars are linear: a stretch is the reader's, who knows its
columns.

## 0. In one screen

- `/.mstream/waves-<gen>.bin` holds, for each card file the writer has peak
  data for, its length and its shape: 240 bars, 0-255. The transfer software
  writes it; the device only reads it.
- It is a card-contract container (M 2.4): magic `MPWF`, **major 1**, a
  224-byte header and one section, `WVSL`, at byte 256. So the file is
  exactly `256 + 256 × rows` bytes, and no slot crosses a 512-byte sector.
- **Slots are sorted by path hash, with no table beside them.** FNV-1a 64 is
  close to uniform, so an interpolation search finds a row in 4 or 5 reads
  (6).
- **Each slot carries a CRC-32 seeded with its row,** as MPDJ's do: the
  device checks what it reads and needs no whole-file pass.
- **A track with no data has no slot** and shows a plain line. The file
  keeps no memory of what was asked, and no content hash.
- **The root names it** in a COMP row after MPTG and MPDJ. A bad row costs
  only itself, never the root (7.2).

## 1. Scope and words

**Reading** needs only this document. **Writing** also follows the commit
and the root (M 2.5, 2.12.1, restated in 7.2), how paths are spelled on the
card (M 2.8.3-2.8.5) and `/.player/device.txt` (M 2.15, restated in 7.4).
MUST, SHOULD and MAY are as in RFC 2119. Sections 11 and 12 are informative.

- **Writer:** the software that copies music to the card (mstream-terminal
  today). **Reader:** the player's firmware, or any program that reads the
  file.
- **Row:** a slot's place in WVSL, from 0. **Peaks:** the server's M bars
  for a track (5). **Bars:** the slot's 240 values made from them.
- **Absent:** a reader behaves as if the file weren't on the card: every
  track shows a plain line. It is never an error the listener must clear.
- **MEASURED-HOST** was run on a PC. **ESTIMATED** wasn't measured.

## 2. Conventions (M 2.3, restated)

- **Bytes:** every integer is little-endian and unsigned. A fourcc is 4
  ASCII bytes in reading order: `MPWF` is `4D 50 57 46`. Reserved fields and
  padding MUST be written as zero, and readers MUST ignore them.
- **CRC-32:** CRC-32/ISO-HDLC, as zlib computes it: reflected polynomial
  0xEDB88320, initial value and final XOR 0xFFFFFFFF. `123456789` gives
  **0xCBF43926**. "CRC-32 over A, then B" is one run over A's bytes and then
  B's: with zlib, `crc32(B, crc32(A))`.
- **FNV-1a 64:** offset basis 0xCBF29CE484222325, prime 0x00000100000001B3;
  for each byte b, `h = (h XOR b) × prime`, mod 2^64.
- **The path hash** of a card file (M 2.3.3) is the FNV-1a 64 of the UTF-8
  bytes of its absolute card path: the literal lowercase `/music`, then `/`
  and each name below it, spelled exactly as the card's FAT directory stores
  it (M 2.8.5); no trailing slash, no NUL. A writer MUST hash the names as
  the card stores them, not as its own OS lists them: NFC and NFD have two
  hashes.

| Input | FNV-1a 64 |
|---|---|
| (empty) | CBF29CE484222325 |
| `/music` | 75DC8A6A38687865 |
| `/music/Artist/Album/01 - Title.mp3` | DA70B312760188D8 |
| `/music/Café/Album/01 - Title.mp3` (é as NFC, `C3 A9`) | F1B24FC757494F2B |
| the same with é as NFD (`65 CC 81`) | 57017C0CC011BB1B |

## 3. The container (M 2.4, restated)

### 3.1 The common header (bytes 0-39)

| Off | Size | Field | Value |
|---|---|---|---|
| 0 | 4 | magic | `MPWF` |
| 4 | 2 | major | 1 |
| 6 | 2 | minor | 0 in this document (9) |
| 8 | 4 | headerBytes | 224. The directory starts here |
| 12 | 4 | sectionCount | 1 |
| 16 | 4 | fileBytes | the file's exact length: 256 + 256 × rows |
| 20 | 4 | generation | the generation of the commit that wrote the file (7.2) |
| 24 | 8 | cardId | the card's id (M 2.5.2) |
| 32 | 4 | headerCrc | CRC-32 of bytes `[0, 256)`, these 4 taken as zero: the header and the directory, so WVSL's CRC too |
| 36 | 4 | reserved | 0 |

### 3.2 The directory: one entry of 32 bytes at byte 224

| Off | Size | Field | Value |
|---|---|---|---|
| 0 | 4 | type | `WVSL` |
| 4 | 4 | flags | bit 0 (value 1): REQUIRED, set here. Other bits are written 0, and readers ignore them (M 2.4.5) |
| 8 | 4 | offset | 256 |
| 12 | 4 | bytes | 256 × rows |
| 16 | 4 | count | rows |
| 20 | 4 | stride | 256 |
| 24 | 4 | crc | CRC-32 of the section's bytes (4.2) |
| 28 | 4 | reserved | 0 |

## 4. The header and the slots

### 4.1 The header (bytes 40-223)

| Off | Size | Field | Value |
|---|---|---|---|
| 40 | 4 | slotBytes | 256 |
| 44 | 4 | barCount | 240 |
| 48 | 176 | padding | zeros up to headerBytes; a later minor's fields start here (9) |

### 4.2 WVSL

- `rows` slots in row order, 1 to 250,000. A writer with no row writes no
  file (7.3).
- Row r's slot is the 256 bytes at `256 + r × 256`. A file starts on a FAT
  cluster, so on a 512-byte sector, and a slot at a multiple of 256 never
  crosses one.
- **The section CRC says nothing the slot CRCs don't.** A CRC-32 run over
  data followed by that data's own CRC ends in a fixed state, so WVSL's CRC
  depends only on `rows` when every slot passes (A 4.3): D35B6D7E for any 7
  slots, BD7B2A5F for any one. It catches damage and a slot at the wrong row
  in one pass. It can't tell two files with good slots apart, and so neither
  can headerCrc nor the root's row. **A writer that needs to know whether
  the contents changed compares the slots' bytes.**

### 4.3 A slot

| Slot offset | Size | Field |
|---|---|---|
| 0 | 8 | `pathHash` (2), u64 |
| 8 | 4 | `lengthMs`: the track's length as the writer knows it, its MPTG record's `durationMs`; 0 unknown |
| 12 | 240 | `bars[0..239]`, u8 each (5) |
| 252 | 4 | `crc`: CRC-32 over the 4 bytes of r as a u32 (little-endian), then the slot's bytes `[0, 252)` |

- **The sort rule:** pathHash strictly increases with r, as an unsigned
  64-bit integer. Hash 0 and hash 2^64 - 1 are valid.
- **Collisions:** a path hash two or more card files share is left out,
  whichever of them have peaks. So a lookup never finds another file's bars.
- **A track with no data has no slot.** A silent track has one, all zero.
- **The seed** makes a slot read for the wrong row fail its CRC. With zlib:
  `crc32(slot[0:252], crc32(r as 4 bytes, little-endian))`.

## 5. The bars

The input is the server's M peak bars `b[0..M)`, each 0-255: the largest
absolute sample over its stretch of the track. `GET /api/v1/db/waveform`
gives M = 800. The rule takes any M ≥ 1:

```
for i in 0..239:
    start = floor(i × M / 240)
    end   = min(M, max(start + 1, floor((i + 1) × M / 240)))
    n     = the sum of b[j]^2 for j in start..end-1
    w     = end - start
    bars[i] = floor(sqrt(n / w) + 0.5)
```

- The floors are integer divisions. n and w are exact integers; `n / w` and
  the square root are IEEE f64.
- **Rounding is half up,** by that sum and floor: a language's `round()` may
  go to even. `sqrt(25 / 4) = 2.5` reads 3. No bar passes 255.
- **M = 800:** bars take 3, 3 and 4 peaks in turn: 0-2, 3-5, 6-9, 10-12 and
  so on, 796-799 last. **M ≥ 240:** the ranges part M with no gap or
  overlap. **M < 240:** every bar takes one peak, and a peak spans 240 / M
  bars or so.
- **Any f64 gets the same bars.** A tie is exact, and anything else is at
  least 1 / (2048 w) from a tie. So an integer reading agrees: a bar is the
  largest v with `w × (2v - 1)^2 ≤ 4 × n`, or 0 when v = 1 is too big.
- **Linear, no stretch.** Loud masters are near-flat here: a reader
  stretches for what it draws (11).

| M | Peaks | Bars |
|---|---|---|
| 1 | `200` | all 200 |
| 2 | `10 20` | 120 × 10, 120 × 20 |
| 7 | `0 50 100 150 200 250 255` | 35 × 0, 34 × 50, 34 × 100, 35 × 150, 34 × 200, 34 × 250, 34 × 255 |
| 241 | `0 1 … 240` | bars[i] = i for i < 239; bars[239] = 240 (RMS 239.5005) |
| 800 | peak 0 is 200, the rest 0 | bars[0] = 115 (sqrt(40000 / 3) = 115.47), the rest 0 |
| 800 | peak 6 is 1, the rest 0 | bars[2] = 1: the tie 0.5 rounds up |
| 800 | peak 6 is 5, the rest 0 | bars[2] = 3: the tie 2.5 rounds up; half to even gives 2 |
| 800 | peak 6 is 253, the rest 0 | bars[2] = 127: the tie 126.5; half to even gives 126 |
| 800 | peaks 6, 7, 8 are 2, 2, 1 | bars[2] = 2: the tie 1.5 from three peaks |
| 1000 | peak 24 is 10, the rest 0 | bars[5] = 4: 25 peaks over 6 bars, this bar takes 5 |

## 6. The lookup

A reader finds a row by the slots alone, with `h` the path hash it wants and
`rows` from the directory. Hashes and the arithmetic of r are u64, and `/`
is integer division. lo and hi are signed: hi reaches -1 when row 0 is above
h.

```
lo = 0;  hi = rows - 1;  loH = 0;  hiH = 2^64 - 1;  probes = 0
while lo <= hi and probes < 24:
    den = (hiH - loH) >> 32
    if probes < 4 and den != 0:
        r = lo + ((hi - lo) × ((h - loH) >> 32) + den / 2) / den
    else:
        r = lo + (hi - lo) / 2
    probes = probes + 1
    read slot r. Its CRC fails: DAMAGED.
    v = its pathHash
    if (lo > 0 and v <= loH) or (hi < rows - 1 and v >= hiH): BROKEN
    if v == h: HIT
    if v < h: lo = r + 1;  loH = v
    else:     hi = r - 1;  hiH = v
MISS
```

- **Interpolation, then bisection.** The first 4 probes place r by the top
  32 bits of `(h - loH) / (hiH - loH)` over lo..hi, rounded to the nearest
  row. A span under 2^32 (den 0) and every later probe bisect.
- **No 128-bit integer.** hi - lo is below 2^18 and `(h - loH) >> 32` below
  2^32, so the sum stays below 2^51. loH ≤ h ≤ hiH always holds, so r stays
  in lo..hi.
- **The first bounds aren't slots.** So the sort check looks at loH only
  when lo > 0 and at hiH only when hi < rows - 1: a slot may hold hash 0 or
  2^64 - 1.
- **24 is a guard.** Four probes and at most 18 bisections are 22 reads, at
  250,000 rows. A lookup that reaches 24 is a miss.

Slot reads a lookup takes (MEASURED-HOST, `tools/mpwf.py`):

| Hashes | Rows | Hit: mean, most | Miss: mean, most |
|---|---|---|---|
| uniform (13.3, `scale.uniform`) | 20,000 | 4.56, 19 | 5.44, 19 |
| paths of one pattern (13.3, `scale.paths`) | 20,000 | 4.78, 19 | 5.64, 19 |
| the synthetic test card's | 19,410 | 4.58, 18 | 5.53, 18 |
| consecutive hashes, the worst shape | 250,000 | most 22 | |

On the test card 84 % of hits take at most 5 reads. The long ones are those
whose 4 probes all fell on one side: they bisect what is left. The mean moves
with the set of hashes, not only with its size: other sets of 20,000 uniform
hashes read 4.25 to 4.99 on a hit and 5.11 to 5.85 on a miss, most 18 or 19
(MEASURED-HOST, `test/test_card_waves`).

## 7. On the card

### 7.1 Name and place

`/.mstream/waves-<gen>.bin`, where `<gen>` is the generation as 8 lowercase
hex digits: `waves-0000002a.bin` is generation 42. It is optional. Only the
transfer software writes it (M 2.11). `waves-*.bin` is a contract name: no
private file may use it (M 2.12.7).

### 7.2 The commit (M 2.5.3, 2.12.1)

A file counts only when the card's root, `/.mstream/manifest.bin` (MSMF),
names it in its COMP section: at most one MPWF row of 40 bytes, which a
writer writes after the MPTG and MPDJ rows, its name after theirs in STRS (M
2.4.4).

| Off | Size | Field | Value |
|---|---|---|---|
| 0 | 4 | kind | `MPWF` |
| 4 | 4 | generation | the file's (older than the root's when a root names the file again) |
| 8 | 4 | fileBytes | its length |
| 12 | 4 | headerCrc | its headerCrc |
| 16 | 4 | name | the root's STRS offset of `waves-<gen>.bin` |
| 20 | 4 | reserved | 0 |
| 24 | 16 | key | zeros |

- **A bad row costs only itself,** unlike MPTG's and MPDJ's and like ACKS (M
  2.5.6). A second MPWF row, or a name that isn't exactly `waves-` + the
  row's generation in 8 lowercase hex digits + `.bin`, leaves the root valid
  with **no waves entry**: of two rows, neither counts.
- **Its name is still a string of the root.** An offset outside STRS, or out
  of STRS's order, fails the root (`string`, `stringOrder`), as for any
  kind.
- **Readers don't check the row's place:** one before the MPDJ row still
  counts.
- **The file is valid** only when the root names it, it opens (8.1, 8.2),
  its generation, fileBytes and headerCrc equal the row's, and the row's key
  is zeros. Otherwise it is absent, and the root stays valid.
- **Firmware that doesn't know the kind** ignores the row (M 2.4.5).
- **The writer's order in a run** (M 2.12.1) is Auto DJ's: `waves-<g>.bin`
  under its final name (nothing names it yet, so a cut leaves the old
  state); then the root through `manifest.tmp` (delete `manifest.bin`, then
  rename); then delete the companions the new root no longer names. **Only
  the final commit names a new file;** a checkpoint names the old one again.

### 7.3 When to write a new file

At the final commit the writer works out **the rows it would write now:**

- **A row** is a card file in the new root's tags file whose bars the writer
  has: from peaks fetched in this run, or in an old slot that still holds. A
  hash two card files share has none (4.3).
- **An old slot still holds** for a record when the file the current root
  names is valid (7.2), the slot's CRC passes, and the record's canonical
  hash (`audio-hash`, else `hash`, M 2.6.6) equals what the tags ledger of
  the root this run began from says for the slot's old path. The file has no
  content hash: the ledger is the only join. So a moved file keeps its bars
  under its new path hash. **When that root is a checkpoint** (FINAL clear),
  the slot's lengthMs MUST also equal the record's durationMs: a checkpoint's
  ledger already shows a recording replaced before it, while the file it names
  again still holds the old bars. A recording of the same length, or of none,
  gets past this, and costs one wrong shape until its path next changes.
- **A row takes** the new path's hash, lengthMs from the card file's own
  MPTG record, and the bars. Its CRC is computed for its new place: one row
  added moves all those after it.

Then the first line of this table that holds decides:

| What holds | Then |
|---|---|
| `read.mpwf` doesn't list 1 (7.4) | no MPWF: the new root names none |
| no row to write | no MPWF: the new root names none. There is no file of 0 rows (4.2) |
| the current root names a valid file, and the rows' bytes equal its WVSL's | the new root names that file again, its generation older than the root's. No commit for this alone |
| anything else | a new file, its header values from this commit |

"The rows' bytes" are WVSL's whole: compare them, not a CRC (4.2). A file the
new root doesn't name goes at the run's clean-up (M 2.12.1, step 13).

**A fetch that stops early has no line of its own.** It decides only which
rows there are, and then the table answers. Nothing gained and no old row
changed: the bytes are equal, and the file is named again. An old row that no
longer holds (its card file left, or a new recording took its place): the
bytes differ, and a new file leaves that row out. No row left: no MPWF.

### 7.4 device.txt (M 2.15)

A writer writes MPWF **only when `/.player/device.txt`'s `read.mpwf` lists
1**, a comma-separated list of majors: `1` and `1,2` do; an empty value and
`2` don't. M 2.15's rules for the file, restated:

- **No file, and no such key,** read as none.
- **The `contract` line is what makes a file a device.txt.** A file without
  one reads as no file, whatever else it holds. The line may stand anywhere,
  and its number doesn't matter.
- **A key given twice** reads as its last.
- **In the list,** spaces in an item are ignored, and an item that isn't a
  number is skipped. A line ends in LF; CRLF is read too.

**`read.mpwf=1` comes with the first firmware that opens the file;** a
firmware before it writes no such key. The firmware writes the key at boot,
after `read.mpdj`. So a card gets its first waves after that firmware has run
once with that card in.

## 8. Validation

A reader MUST treat the file as absent when a check of 8.1 or 8.2 fails. The
words in parentheses are the reference reader's reasons (8.4, 13.5).

### 8.1 The frame (M 2.4.3), in this order

1. The file holds at least 40 bytes (short).
2. magic is `MPWF` (magic).
3. major is 1 (major). Any minor is accepted.
4. headerBytes is a multiple of 8 and at least 224 (headerBytes).
5. sectionCount ≤ 64 (sectionCount).
6. headerBytes + 32 × sectionCount ≤ the file's length (short).
7. fileBytes equals the file's length (fileBytes).
8. headerCrc matches, over `[0, headerBytes + 32 × sectionCount)`
   (headerCrc).
9. The directory, entry by entry: the offset is a multiple of 8, at or after
   the end of the section before it (the directory's end for the first),
   less than 8 bytes past that end, and offset + bytes ≤ fileBytes; the type
   hasn't appeared before (layout). Then an unknown type with bit 0 of its
   flags set (REQUIRED) makes the file absent (unknownRequired), and one with
   bit 0 clear is skipped; the other bits are ignored. For WVSL: the stride is
   at least 256, and bytes = count × stride (shape).
10. The last section ends at fileBytes (layout).
11. WVSL is present (missing).

**Without overflow:** the sums and the product of steps 6, 9 and 10 MUST be
computed without wrapping: in 64 bits, or with an explicit check. Each can
pass 2^32 with u32 fields. 13.5's `mpwf-*-wrap` files are such directories.

### 8.2 The header and WVSL, in this order

1. slotBytes is 256 and barCount is 240 (enum).
2. WVSL's stride is 256 (shape).
3. 1 ≤ WVSL's count ≤ 250,000 (enum).
4. WVSL's offset is 256 (wvLayout).

With 8.1, step 4 makes WVSL the one section and the file exactly `256 + 256
× rows` bytes: only headerBytes 224 and one entry end the directory at 256,
and 8.1's step 10 ends the file with WVSL.

### 8.3 A slot

Each slot a reader uses is checked in this order:

- **W1.** Its CRC (4.3) matches, seeded with the row asked for (wvSlotCrc).
- **W2.** Its hash keeps the sort rule (wvSlotOrder). A whole-file check
  compares it with the row before; a lookup with the bounds it has read (6).

A reader that verifies the whole file treats any slot failure as absent. A
reader that checks slots as it reads them, as the device does:

- W1 fails: the slot is **damaged** (the card's fault). This lookup misses,
  and the file stays. The reader counts it.
- W2 fails under a good CRC: the slot is **broken**, the work of a broken
  writer. The file is absent from then on.
- A read that fails (an I/O error) is neither: the file isn't to blame.

### 8.4 The check order

So that every reader gives the same first reason, a reader that verifies
runs 8.1 and 8.2; then, when it uses every slot, each slot in row order, W1
then W2; and last WVSL's section CRC (sectionCrc). The first failure is the
reason. The reference tools have two sets of uses: **all** (every step) and
**device** (8.1 and 8.2; slots are checked as they are read, and the section
CRC not at all).

### 8.5 Not checked by readers (writers MUST get them right)

- That bytes 48-223 and the reserved fields are zero. headerCrc covers them.
- WVSL's REQUIRED flag: a reader that knows the type doesn't look.
- That every row is a card file, that lengthMs matches the tags file, that
  the bars follow section 5: only a recomputation can tell. A checkpoint may
  name an old file whose rows have fallen behind (7.2); the final commit puts
  that right (7.3).

## 9. Versions

- **Major 1** is this document. A reader of another major treats the file as
  absent.
- **The writer's guard** (M 2.4.5): `waves-*.bin` is on the list of contract
  files whose magic and major a writer checks before it writes. A writer of
  this document knows major 1 of MPWF, and stops on any other.
- **A writer that doesn't know MPWF** drops the row at its next commit, and
  the file stays until a newer run's clean-up (M 2.12.1, step 4). That is
  legal: everything in the file can be derived again (M 2.4.5).
- **A minor** only adds, and readers accept any minor. Bytes 48-223 of the
  header are all it has: every addition MUST mean "absent" as all zeros, and
  MUST be derivable again.
- **These need a new major:** a new section of any kind (8.2's step 4 would
  make the file absent: 13.5's `mpwf-unknown-optional`), a longer header, a
  longer slot, another bar count, a new bars rule, any existing field's
  meaning or position, the sort rule.

## 10. Limits and sizes

rows: 1 to 250,000, so the file is under the transfer's 64 MiB file cap. No
device reads so many: its PSRAM tops out near 23,000-27,000 tracks (M 3.5).
`python tools/mpwf.py size ROWS` computes `256 + 256 × rows`:

| Rows | fileBytes |
|---|---|
| 1 | 512 B |
| 10,000 | 2,560,256 B (2.56 MB) |
| 20,000 | 5,120,256 B (5.12 MB) |
| 50,000 | 12,800,256 B (12.80 MB) |
| 250,000 | 64,000,256 B (64.00 MB) |

That is half the AutoDJ index's 10.6 MB at 20,000 (A 10.2). A WiFi resend
takes about 26 s at 0.2 MB/s and 7 s at 0.7 MB/s (ESTIMATED).

## 11. What the device does (informative)

Nothing here is part of the format: it is what the job behind `read.mpwf=1`
does (7.4). docs/SEEK-BAR.md has the bar's look.

- **On a track change,** Now Playing wants that track's path hash. A worker
  job in the covers' lane opens the file, checks the frame and the root's
  row (8.1, 8.2, 7.2), runs the lookup, closes the file and reduces the
  bars. No handle is kept between jobs. About 20 ms (ESTIMATED). An answer
  for a track no longer wanted, or from before a remount, is dropped.
- **One record** is kept, whichever hash it answers for: the shape, or that
  there is none (a miss, a damaged slot). So a repaint and a library update's
  new ids read no card.
- **A shape shows** only when its length agrees with the player's (the bar's,
  frozen at the Down, under a finger): lengthMs 0 does, otherwise `|lengthMs
  - player's ms| ≤ max(2000, player's ms / 50)`. A track too short to seek, or
  of no known length, or that failed never shows one. Anything else is the
  plain line, in the same place.
- **The reduction to 99 columns** (`lib/core/WaveShape`): each column is the
  RMS of its bars, then a stretch from the 10th percentile column to the
  loudest. At face value loud masters are a brick.
- **Failures:** damaged is a plain line for this track. Broken, or a failure
  of the file, a file that isn't on the card included, is one log line and
  plain lines until a remount or the next boot, and a shape read before goes
  too. A read's I/O error, one in the file's open too, is retried once after
  2 s, then no shape until the track changes.
- **The console:** `gw?` prints the state (the file, the wanted hash, the
  record, the counters: reads, hits, misses, damaged, the probes a read, the
  job's ms); `gw=` and 99 digits, or `gw#` and 480 hex digits (240 bars,
  through the reduction), set a test shape with no card, until `gw-`. `gw` alone
  is the console's walk of the card, as it was.

## 12. Writer guidance (informative)

**Which tracks.** 7.3 says what a row is and when an old slot still holds.

- **The file keeps no "asked, none".** A track the server had no peaks for
  has no slot, and a later run may ask again.
- **What has no row in v1:** a peer's file, unless the server has the same
  recording (its peaks are the recording's, by the canonical hash),
  hand-copied files, files edited on the card. A converted file takes its
  source's peaks, and lengthMs from the card file's record.

**What a commit costs.** The file costs 256 bytes a track. A commit makes
the device build its library again at its next boot (M 2.5.6), 7-9 s at
20,000 tracks. So a run makes no commit for the waves alone unless the bytes
change (7.3). A card that lacks waves and has nothing else to do must still
be able to get them: that run commits, because its bytes change.

**A soft failure.** A fetch can stop: the listener stops it, a token
expires, the network goes, a time budget runs out. That is never a failed
transfer. Keep what came, with the old rows that still hold, and let 7.3's
table answer. **Never delete the old file while it names useful rows:** a run
with no answers isn't a card with no shapes. This differs from Auto DJ's no
vectors, no index. Only when no row is left is there no file, as there. Say so
in a note; the next run asks for the rest.

**A long fetch.** A fetch can take half an hour, and it comes ahead of the
final commit's first write. A writer that copied files since its last
checkpoint makes one more before it asks (M 2.12.1's step 9, after the last
file), so a card pulled in that wait keeps them.

**Two writers, one file.** They agree byte for byte when they agree on which
tracks have rows, the paths as the card stores them, the peaks, lengthMs,
generation and cardId. The fixtures (13) pin the rest.

## 13. Test vectors and fixtures

### 13.1 Where

In the player repo's `test/fixtures/card/`, under CC0 (its README; M 2.17):
`libraries/<name>.json`, the descriptions (13.2); `golden/<name>.bin`, what
a writer MUST make of each, byte for byte; `hardening/mpwf-*.bin` and
`hardening/msmf-waves-*.bin`, listed in `hardening/index.json` (13.5); the
`"mpwf"` group of `vectors.json` (13.3). `tools/card_fixtures.py` makes
them, with `tools/mpwf.py`, and keeps them frozen (`--check`). `python
tools/test_mpwf.py` tests the reference. Every name, hash and vector in them
is made up. `tools/synthtransfer.py --waves` gives a test card a file of
made-up shapes, seeded.

### 13.2 The description format

```
{
 "format": "MPWF", "file": "<golden name>.bin",
 "major": 1, "minor": 0,                     optional, these defaults
 "generation": 42, "cardId": "0x5EEDC0DE0A1B2C3D",
 "note": "...",
 "tracks": [ {
   "path": "<relative to /music, as the card stores it, no leading slash>",
   "hash": "0x...",                          instead of path: the hash itself
   "lengthMs": 180000,
   "peaks": [ 0, 255, ... ],                 the M peaks, M >= 1
   "note": "..." } ]
}
```

Tracks may come in any order; the writer sorts them. A hash two tracks share
is left out, both tracks. `python tools/mpwf.py build` also takes a plain
`{path: {lengthMs, peaks}}`, with `--generation` and `--card-id`.

### 13.3 The primitives: vectors.json "mpwf"

| Key | What it holds |
|---|---|
| `names`, `sizes` | generation → name; rows → fileBytes |
| `barRanges` | `{m, cases}`, cases `{i, start, end}`: 5's ranges |
| `bars` | `{m, peaks, bars, note}` in hex, or `{m, peaksAt, barsAt, note}` where each map is `{index: value}` and every other value is 0: 5's table and more |
| `slot` | waves-one.bin's slot: its hash, lengthMs, bytes, CRC 0xB0EF4F1C, and the CRC of the same 252 bytes without the seed, 0x1B1A93B5 |
| `slotCrc` | the same for waves-0000002a.bin's row 3: 0x2977DAF4, unseeded 0x0BA69241 |
| `sectionCrc` | `{rows, file, crc}`: 4.2's constants |
| `pick` | `{h, lo, hi, loH, hiH, probes, row, note}`: one step of 6's r, den 0 included; `probes` is the variable as that step starts: the probes already made |
| `search` | `files`, by file, `{hash, read, row, probes, note}`: a lookup's answer and the rows it reads in order |
| `scale` | two sets of hashes built by rule, with the reads summed over every hit and miss. `uniform` is SplitMix64 from state 0, 20,000 rows then 5,000 misses; `paths` is the path hash of `pattern` over 400 artists, 5 albums and 10 tracks, and of `missPattern`. The same search gets these sums |
| `rowLimit` | `{rows, fileBytes, head, why}`: 250,000 rows open, 250,001 read `enum`, as the device reads them (8.4). `head` is the first 256 bytes: a source of that length needs no 64 MB of slots. WVSL's CRC in it is 0, and headerCrc is right for that: no reader of `head` reads the slots, so a writer's file of those rows has other bytes at 32 and 248 |
| `deviceTxt` | `{who, cases}`: 7.4's cases (who: software), each `{readMpwf, writes}` (the value under a contract line), `{noFile, writes}`, `{keyMissing, writes}` or `{text, writes, note}` (the file's whole text) |
| `commit` | `{who, cases}`: 7.3's table (who: software), each `{readMpwf, old, rows, outcome, note}`. `old` is the golden file the current root names, or null. `rows` is the golden file whose rows the writer starts from, or null; less `drop` (row numbers), with `lengthMs` (`{row: value}`) applied, then `add` (`{hash, lengthMs, peaks}`), sorted by hash. `outcome` is `none`, `same` or `new` |
| `rootRow` | what a reader takes from `manifest-waves.bin` and the `msmf-waves-*` files: the row's name or null, and whether it matches the golden file of that name |

### 13.4 The goldens

**waves-one.bin** is the smallest file: generation 1, one row, M = 1, every
bar 0x40. The slot: pathHash DA70B312760188D8 (`Artist/Album/01 -
Title.mp3`), lengthMs 180,000 (`20 BF 02 00`), the bars, crc 0xB0EF4F1C.

```
00000000  4d 50 57 46 01 00 00 00  e0 00 00 00 01 00 00 00  |MPWF............|
00000010  00 02 00 00 01 00 00 00  3d 2c 1b 0a de c0 ed 5e  |........=,.....^|
00000020  26 ac d6 9d 00 00 00 00  00 01 00 00 f0 00 00 00  |&...............|
00000030  00 00 00 00 00 00 00 00  00 00 00 00 00 00 00 00  |................|
*
000000e0  57 56 53 4c 01 00 00 00  00 01 00 00 00 01 00 00  |WVSL............|
000000f0  01 00 00 00 00 01 00 00  5f 2a 7b bd 00 00 00 00  |........_*{.....|
00000100  d8 88 01 76 12 b3 70 da  20 bf 02 00 40 40 40 40  |...v..p. ...@@@@|
00000110  40 40 40 40 40 40 40 40  40 40 40 40 40 40 40 40  |@@@@@@@@@@@@@@@@|
*
000001f0  40 40 40 40 40 40 40 40  40 40 40 40 1c 4f ef b0  |@@@@@@@@@@@@.O..|
00000200
```

**waves-0000002a.bin** is the fixture card's, generation 42, 2,048 bytes:
seven records of `tags-0000002a.bin`, each lengthMs that record's
durationMs. The card's other records have no slot.

| Row | pathHash | Path | lengthMs | Peaks | Bars |
|---|---|---|---|---|---|
| 0 | 57017C0CC011BB1B | `Café/Album/01 - Title.mp3` (NFD) | 193,000 | 30, 40, 50 | 80 of each |
| 1 | 6137B0E2A6715752 | `Artist/Album/CD1/01 - Disc One.flac` | 183,000 | M = 800, a made-up song | peak 239 |
| 2 | 954A7DE3A079A42E | `A/x.mp3` | 186,000 | 0 | all 0: silent, and it has a slot |
| 3 | C48674AC937F2ADA | `Artist/x.mp3` | 0 (the record has none) | 255 | all 255 |
| 4 | CADB3EFDEF4B2DD9 | `Artist/Album/02 - Other.mp3` | 181,000 | M = 800: peak 10k + 6 is 2k + 1 for k 0-79, the rest 0 | bars[3k + 2] = k + 1, every one a tie; the rest 0 |
| 5 | DA70B312760188D8 | `Artist/Album/01 - Title.mp3` | 180,000 | M = 800: `j × 255 / 799` | a ramp to 254 |
| 6 | F1B24FC757494F2B | `Café/Album/01 - Title.mp3` (NFC) | 192,000 | 10, 20 | 120 of each |

**waves-skew.bin** is a search test: generation 9, 49 rows, hashes given:
0-39, then 0x4000000000000000, then 2^64 - 8 to 2^64 - 1. A lookup of hash
20 reads rows 0, 1, 2, 3 (num is 0, so r = lo, four times), then bisects:
26, 14, 20. Its description lists two more tracks with one hash,
0x8000000000000000, left out.

**manifest-waves.bin** is a root of generation 43 that names
`tags-0000002a.bin`, `autodj-00000029.bin` and `waves-0000002a.bin`
(generation 42, carried). `manifest.bin` itself names no waves.

### 13.5 Hardening files

Each file breaks one rule of section 8 and keeps everything else valid:
every section CRC and the header CRC are made right, and a slot edited on
purpose has its CRC made right unless the CRC is the fault.
`hardening/index.json` lists each with its uses (8.4), expect (absent or
present), why, `same` (a present file's golden: every slot's hash, length
and bars read the same) and `slot` (one lookup as the device makes it:
`{hash, read, reads}`). All are made from waves-0000002a.bin.

| Reason | Files, and what is broken |
|---|---|
| short | `mpwf-short` 39 bytes; `mpwf-short-directory` 200 bytes; `mpwf-header-bytes-wrap` headerBytes 0xFFFFFFF8 (wrapped to 32 bits the directory ends at 24, and the CRC is right for that) |
| magic | `mpwf-magic` MPWX |
| major | `mpwf-major-0`; `mpwf-major-2` |
| headerBytes | `mpwf-header-bytes-short` 216; `mpwf-header-bytes-odd` 228 |
| sectionCount | `mpwf-section-count` 65 |
| fileBytes | `mpwf-file-bytes` 8 too many; `mpwf-truncated` 100 bytes cut |
| headerCrc | `mpwf-header-crc` |
| layout | `mpwf-layout-gap` WVSL at 264; `mpwf-layout-odd-offset` at 260; `mpwf-layout-overlap` at 248; `mpwf-layout-trailing` 8 bytes after it; `mpwf-wvsl-twice`; `mpwf-section-end-wrap` an optional section of 0xFFFFFF00 bytes before it; `mpwf-truncated-resealed` 100 bytes cut, fileBytes and the CRC made to agree |
| unknownRequired | `mpwf-unknown-required` WVZZ after WVSL; `mpwf-unknown-required-flags` the same with flags 3: bit 0 and a bit no reader knows |
| shape | `mpwf-stride-short` 128; `mpwf-slot-stride` 512; `mpwf-count-bytes` count 8, bytes for 7; `mpwf-array-bytes-wrap` count 7 + 2^24 (wrapped to 32 bits, count × stride is right) |
| missing | `mpwf-missing-wvsl` |
| enum | `mpwf-slot-bytes` 512; `mpwf-bar-count` 120; `mpwf-rows-zero`; 250,001 rows is `rowLimit` (13.3) |
| wvLayout | `mpwf-misaligned` headerBytes 232, WVSL at 264; `mpwf-header-bigger` headerBytes 480, WVSL at 512; `mpwf-wvsl-not-first`; `mpwf-unknown-optional` an optional section after WVSL; `mpwf-unknown-optional-flags` the same with flags 2: bit 0 clear, so not REQUIRED |
| wvSlotCrc | `mpwf-slot-crc` slot 2; `mpwf-slot-wrong-row` slots 1 and 2 swapped, each CRC right for its own row; `mpwf-flip-hash`, `-length`, `-bar`, `-crc` one bit of slot 3. Their lookups read damaged |
| wvSlotOrder | `mpwf-slots-unsorted` slots 1 and 2 swapped, CRCs made right; `mpwf-slots-equal` slots 2 and 3 hold one hash. Their lookups read broken |
| sectionCrc | `mpwf-section-crc` |

Present, each the same as waves-0000002a.bin unless said:

| File | What it holds |
|---|---|
| `mpwf-minor-1` | minor 1, bytes 48-223 not zero |
| `mpwf-reserved-set` | minor 0, the reserved fields and a padding byte set |
| `mpwf-wvsl-not-required` | WVSL's REQUIRED bit clear |
| `mpwf-truncated-row` | the last slot cut and every field made to agree: a valid file of 6 rows, not the same. The root's row tells them apart |
| `mpwf-uses-device-slot-crc` | read by `device`: a lookup reads damaged |
| `mpwf-uses-device-unsorted` | read by `device`: a lookup reads broken |
| `mpwf-uses-device-section-crc` | read by `device`, which doesn't check it: a lookup hits |

**The root's row.** These roots read, in firmware that doesn't know the kind
too. A reader takes the row of `msmf-waves-no-mpdj`, `-between` and `-first`;
no waves entry from `-name-upper`, `-name-generation`, `-name-stem`,
`-name-empty` and `-two`; the row of `-key`, `-file-bytes` and `-header-crc`,
whose file then doesn't match (`rootRow`, 13.3). `msmf-waves-name-range`
(string) and `msmf-waves-name-order` (stringOrder) make the root absent: a
string rule, as for any kind (7.2).

### 13.6 How they were checked (2026-11-02)

- `tools/test_mpwf.py` rebuilds every golden, recomputes the bars by the
  integer reading (5) and the lookup's answers by a walk from row 0, and
  reads every hardening file to index.json's verdict, reason and lookup.
  Every cut and every flipped bit of waves-one.bin and waves-0000002a.bin
  reads absent to `all`: this file has no gap and no byte outside a CRC. To
  `device`, only a flip in the first 256 bytes does.
- **A throwaway second implementation,** in C with u32 and u64 and no
  128-bit type, written from this document, rebuilt the three goldens byte
  for byte, read every MPWF hardening file to the same reason and lookup,
  and got every `bars`, `pick`, `search` and `scale` vector.
- The player's reader and host writer (`lib/core/CardWaves`) were written
  from this document and the fixtures, before their author read the
  reference, and are tested against the same fixtures in
  `test/test_card_files`, `test/test_card_contract` and
  `test/test_card_waves`: they make the goldens byte for byte, read every
  hardening file to its reason and lookup, and read the `scale` sums exactly.
