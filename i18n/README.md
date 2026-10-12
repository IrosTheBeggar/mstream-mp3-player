# The UI's texts

Every text the player draws, one file a language: `en.json` is the
source, and each translation is `<tag>.json` beside it (`de.json`,
`pt.json`, ...). The languages are the mobile app's and the terminal
player's: de, es, fr, it, ja, pl, pt (Brazilian), ru and zh (Simplified).
All ten are on the player's Language page (Output > Language), and each
has its file: machine translations, made from the two apps' words where
they had them and by Fable agents for the rest, **not yet reviewed by a
native speaker** (a review is welcome: below). A text a translation
leaves out is drawn in English (as a language with no file would be).
How it all works, and how the translations were made, is in
[docs/I18N.md](../docs/I18N.md), phase 3.

`tools/gen_ui_strings.py` reads the files, checks them, and makes the
firmware's tables (`lib/core/UiStringIds.h`, `lib/core/UiStringsData.cpp`)
and the width tests' view of them (`test/test_ui_strings/catalogue.h`):

    python tools/gen_ui_strings.py            # check and regenerate
    python tools/gen_ui_strings.py --report   # how much of each language there is
    python tools/gen_ui_strings.py --dump de  # every text of a language, one a line
    pio test -e native -f test_ui_strings     # every text's glyphs, buffers and rooms, in every language

Commit the three generated files with the catalogue; CI fails when they
are out of date (`--check`).

## A translation file

```json
{
  "@@locale": "de",
  "@@autonym": "Deutsch",
  "coach.title": "Die drei roten Punkte",
  "library.tracks_n": {"one": "{0} Titel", "other": "{0} Titel"}
}
```

- `@@locale` is the file's tag; `@@autonym` the language's name in
  itself, as the mobile app writes it (English, Deutsch, Español,
  Français, Italiano, 日本語, Polski, Português, Русский, 中文: the
  generator checks it; the Language page lists it so, never translated).
- `@@note` says how the file was made (below).
- Each id is `en.json`'s. An id it doesn't have fails the check; one
  left out is English on the device.
- `"@coach.title": {"note": "..."}` may say why a text is as it is; the
  generator ignores it. `"overflow": true` there marks a text that is too
  wide in this language for now (its room needs a layout fix): the width
  test lets it be, and fails once it fits (then the mark goes).
- UTF-8, no BOM. Keep `en.json`'s order: a diff then reads as a list.

## What en.json says about each text

After each text, `@<id>` holds what a translator needs:

- `context`: where it is and what it does.
- `room`: the room the screen gives it: the font (`Body` DejaVu Sans
  16, `Small` 13, `Bold` Bold 16, `Title` Bold 22), the pixels (`px`),
  and `lines` when the layout places several lines one by one. `else`
  is the smaller font the code falls back to when the first doesn't fit.
  `layout` is `wrap` for a text the code wraps over its lines itself (a
  dialog's body), and `toast` for a note (one line in Body; else split
  at the first `": "`, the part before it in Small over the rest, each
  `two_line_px` wide). `optional`: the code draws it only when it fits;
  otherwise another text, or nothing (a tab's label: the tab shows its
  icon alone; the toast's Undo and View: their icons). A list of rooms:
  the text is drawn in each.
  `null`: a piece another text holds (its room is that one's).
- `placeholders`: `{0}` to `{3}`, what fills each and its worst case
  (`type`: a count written `grouped` or as plain `digits`, a `decimal`,
  a `text` such as a name, or another `message`), and in `or` the other
  texts that may fill it: the tests try each in your language.
- `count`: the placeholder that picks a plural text's form.
- `bytes`: the buffer the text goes through on the device, filled (its
  UTF-8, plus one): a Cyrillic letter takes two bytes, a CJK character
  three. `line_bytes`: the same for each line. A text that doesn't fit
  fails the check (the generator for a text with no placeholders, the
  host test for the rest): shorten it, or the buffer grows in the code.
- `translate`: `maybe` for technical words a language may keep
  (CPU, PSRAM, BPM, MHz, the reset reasons), `dev` for what only a
  developer sees (may stay English).
- `overflow`: too wide already in English (a layout fix is to come);
  the tests let these be, in every language. None is now: the layout
  fixes of phase 3 made room for every language's texts.

## The rules

**Placeholders.** `{0}` to `{3}` are the only special characters: keep
each one the English has (in any order, as the language wants), and add
none. `%` is a percent sign; a brace is never anything else.

**Lines.** `\n` is a line break the screen keeps, only in a text whose
room has `lines`; each line must fit the room, and there are at most
that many. Break where the language reads well.

**Plurals.** A plural text is an object of forms by CLDR's categories,
with the language's rule picking one by the count (CLDR 46):

| Language | Forms | Needed | `one` is |
|---|---|---|---|
| en, de | one, other | both | 1 |
| es, it | one, many, other | one, other | 1 |
| fr, pt | one, many, other | one, other | 0 and 1 |
| pl | one, few, many, other | one, few, many | 1 |
| ru | one, few, many, other | one, few, many | 1, 21, 31... (not 11) |
| ja, zh | other | other | - |

`many` in es, fr, it and pt is for whole millions, and `other` in pl and
ru for fractions: no count here reaches them, so they may be left out
(`other` stands in). Where `one` is more than the number 1 (fr, pt, ru),
its form says the count (`{0}`), never a word for one. Every form keeps
the English's placeholders.

**Notes.** A note splits at its first `": "` onto two lines when it is
too wide for one: keep `": "` there (`"Ajouté : {0}"` with a no-break
space before the colon is fine).

**Characters.** The fonts have Latin-1, Latin Extended-A, Greek,
Cyrillic, the typographic punctuation (– — ‘ ’ “ ” „ … « » ‹ › •), and
Chinese, Japanese and Korean at 16 px: the CJK store's kana, JIS X
0208's kanji, GB2312's level 1 hanzi, Hangul, and CJK Symbols and
Punctuation (、 。 「 」 『 』 〜). A character neither has fails the
generator's check, which names every one at once (U+XXXX and the ids
that use it), and `test_every_text_has_its_glyphs`: write another, or
add it to the store (`tools/glyph_store.py` takes every character of
`i18n/*.json`; it needs the CJK font, and the store is committed with
the file). **No fullwidth forms** (U+FF01-FF5E: ， ： （ ） ！ ？ ；):
the device folds them to ASCII before drawing, so they fail the check
too. In Chinese and Japanese write the ASCII sign (`,` `:` `(` `)` `!`
`?`, a space after where it reads better), or the store's 、 。 「 」;
the mobile app's and the terminal player's texts use the fullwidth
ones throughout, so change them when you take their words. No halfwidth
katakana, and `-` for a minus. The no-break space is U+00A0: the narrow
one (U+202F) isn't in the fonts, and a plain space may break a line
there.

**Numbers.** The code writes the counts, with the language's thousands
mark (`19.410`, `19 410`) and decimal mark: a text holds `{0}`, not
digits. Units (min, s, MHz, kB) are the translator's to write as the
language does.

**Names.** Keep what names a thing: FAT32, exFAT, NTFS, GPT, MBR, the
paths (`/music/Artist/Album/`), GPL-3.0-or-later, Opus, FLAC, MP3, Ogg,
Bluetooth, Core2. Texts that name other texts (`Output > About`, `tap
Try again`) use those texts' words in the language.

**Words.** One word for one thing across the UI, and the mobile app's
(mstream_music's `lib/l10n/app_<lang>.arb`) where it has the thing, so
the two read alike: its register too (de du, fr vous, es tú, ru вы, pt
Brazilian). The terminal player's MP3-player tab (`locales/<lang>.yml`)
has the device's words (card, firmware, headphones, battery).

## Credit

A file says in `@@note` how it was made: a machine translation (as the
mobile app's and the terminal player's are) says so, and names what it
took from those two and from which commit (both GPL-3.0, the same
author's; THIRD-PARTY-NOTICES.md, "The UI's translations"). A native
speaker's review is welcome, as a pull request on the language's file:
say in `@@note` that it was reviewed, and where a word the mobile app or
the terminal player uses too should change, say so in the pull request,
so the apps can change with it and mStream's apps keep the same words.
