# N11's synthetic card, as the walk sees it

`walk-shape.txt`: the folders of `tools/synthcard.py`'s default tree (its
plan, seed 1: the 20k card the device ran on 2026-10-08) as the validation
walk (lib/core `CardWalk`) lists them: one line per folder, in pre-order,
`depth audio images others`, with what the walk leaves out left out (names
starting with ".", paths over 248 bytes below `/music`, folders past 8
levels). 2,591 folders (`/music` included), 19,410 audio files, 1,688
images, 1,570 other files: the device's counts. No names: `test_card_jobs`
makes them up, which changes nothing the walk counts (every folder fits
its 64 KB scratch either way).

Made by `walk_shape.py` (`python -I test/fixtures/synthcard/walk_shape.py
. test/fixtures/synthcard/walk-shape.txt`): it imports `tools/synthcard.py`
and builds the plan, writing no card. Given a dump of a card's tree too
(`D<TAB>folder` and `F<TAB>file<TAB>size` lines, paths relative to
`/music`), it says whether that card has the same shape.

`test_card_jobs` walks it: a step a folder (2,593 steps, where the
device's one-move-a-step walk took 7,774), in slices on a fake clock
(docs/METADATA.md 3.2.3, 3.3.9).

# N11's synthetic card's names

`names.txt`: 4,588 names from the same plan, one a line (made by
`names.py`: `python -I test/fixtures/synthcard/names.py .
test/fixtures/synthcard/names.txt`): every artist folder's name, every
artist, album artist, album and sort tag value, every 4th album folder's
name, every 40th title, every 80th file name, every 4th accented one, and
every name in another script. All made up by `tools/synthcard.py`
(invented syllables, common English words, its accents, its made-up
Cyrillic, Greek, kana and Hangul strings). `test_text_fold` sorts them by
the order of c97ff3c and by today's: the names the old folding could spell
keep their order (docs/I18N.md, phase 0).
