# Tag reader parity corpus

SPDX-License-Identifier: CC0-1.0. To the extent possible under law, the
authors have waived all copyright and related or neighboring rights to the
files in this folder (the CC0 1.0 Universal dedication,
https://creativecommons.org/publicdomain/zero/1.0/), so mstream-terminal can
copy them (docs/METADATA.md part 7, U18, proposed). The player's code stays
GPL-3.0-or-later.

docs/METADATA.md 2.17, item 3: synthetic audio files, each a few of part 5's
reading rules, and the record each must give.

- The files: made by `tools/tag_corpus.py` (`--check` fails if one here
  differs). Every name in them is made up; the audio is a few silent
  frames, the pictures a few bytes that only start like a JPEG or a PNG.
- `anchors.json`: per file, where each embedded picture's bytes are (2.6.4's
  anchor: offset, stored length, picCoding) and the FNV-1a 64 of its image data.
- `expected.json`: the record each file gives, made by the reference reader
  `tools/tagref` (lofty 0.25, mStream's rust-parser selection rules, part 5
  and 2.3.6), with the elected picture's anchor from anchors.json. A reader
  of the contract matches it field for field, the length within 100 ms
  (lofty doesn't trim the MP3 encoder delay; the device does).

The player's test: `test/test_tag_scan` (TagScan against expected.json,
then mutated copies of every file for the fuzz pass).

Where the device's reader knowingly differs from lofty (all rare, and none
of these in this corpus): a compressed ID3v2 text frame (lofty inflates it),
the repair pass's fallback for a tag with no padding to grow into, COVERART,
an APE tag at the head of an MP3, a Lyrics3 block before ID3v1, the base64
of an Opus picture past its head, a frame no field comes from repeated
within one tag (lofty's list may replace it; the device only counts it), a
Vorbis value whose bytes stop being UTF-8 after its first 4 KB, more than 96
frames or comments in one tag that a field may take (the `many_*` files here
pass 96 with entries the device can let go of), and tags past the device's
read budget (an Ogg comment packet of more than about 500 pages, an
unsynchronised v2.2/2.3 tag past 8 MB: the device keeps what it found).
Then the files lofty fails on (the two `lofty_fails_*` here): the reference
reads them as UNREADABLE and the device reads them itself (2.9).
