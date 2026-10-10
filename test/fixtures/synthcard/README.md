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
