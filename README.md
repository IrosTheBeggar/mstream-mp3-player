# mstream-mp3-player

An experimental portable MP3/FLAC player on the **M5Stack Core2** that will
keep a copy of part of your [mStream](https://mstream.io) library and sync it
over WiFi. Listening is Bluetooth-first (A2DP headphones), with the built-in
speaker as a fallback.

> **Status:** proof of concept. Plays MP3 and FLAC from the SD card (or the
> Core2's internal flash) to Bluetooth headphones or the speaker. The UI is a
> tab bar (Now Playing, Library, Queue, Dance, Output), all five built:
> Now Playing with the album cover, the Library (artists, albums with
> covers, folders, the A-Z jump grid), the Queue (editing, Undo), Output
> (Bluetooth pairing, per-output volume, settings, About); library sync and
> AutoDJ come next. The library is indexed from the card (and cached there), and the
> play queue survives a restart. Measurements: [docs/POC-RESULTS.md](docs/POC-RESULTS.md).
> Design: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Hardware

M5Stack Core2 (tested on v1.3): original ESP32 (dual core, 240 MHz), 16 MB
flash, 8 MB PSRAM (4 MB usable), AXP192 power chip, 320×240 touch LCD with
three touch buttons, 1 W mono speaker, microSD. There is no headphone jack:
use Bluetooth headphones, the speaker, or (later) M5Stack's RCA/3.5 mm module.

## Quick start (Windows)

1. **PlatformIO Core 6.2.0 or newer:** `pip install -U platformio`.
2. **Build and flash from PowerShell** (not Git Bash: the ESP-IDF tools that
   pioarduino uses refuse to run under MSYS). The first build downloads the
   pioarduino platform and toolchain (about 1 GB) and takes a while.

   ```powershell
   Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue
   pio run -e core2 -t upload
   ```

   Each build ends with a `flash_guard: ok` line (see
   [Installing and updating](#installing-and-updating)). A Core2 flashed
   before the current layout (a 4 MB `factory` app, settings at 0x9000)
   needs [moving to it](docs/ARCHITECTURE.md#moving-an-existing-unit-to-the-new-layout)
   once, or it loses its settings.

3. **Test audio.** Make known-pitch test files, plus optional excerpts of your
   own tracks, then write them to the Core2's flash filesystem (3.8 MB; a
   real library goes on the SD card):

   ```powershell
   python -m venv .venv-tools
   .venv-tools\Scripts\pip install imageio-ffmpeg
   .venv-tools\Scripts\python tools\make_test_audio.py --mp3 song.mp3 --flac song.flac
   pio run -e core2 -t uploadfs
   ```

4. **Headphones.** Pair them on the device: put them in pairing mode, open
   **Output > Pair new headphones**, and tap them in the list (each audio
   device nearby with its kind and signal). The Core2 connects, moves the
   music to them and remembers them: after a drop or a restart it pages
   them (3 tries, then now and then, resting after 15 min), and they can
   also reconnect by themselves whenever they wake up. Until some are
   paired the Bluetooth card says "No headphones paired", and a B hold or a
   play on Bluetooth only points there.

   **It never pairs by itself.** With no headphones paired the Core2 doesn't
   scan at all (at the boot, on Connect, a B hold or a play): it stays
   quiet, connectable only, and never picks a device for you, by name or by
   signal strength. (An early build took the strongest audio device when
   no name was set, and once paired a TV in the next room.) Remembered
   headphones are only ever paged, never scanned for.

   For development, a build can scan by name while none are remembered:
   set the name in a gitignored `local.ini`

   ```ini
   [local]
   build_flags = -DBT_SINK_NAME=\"My Headphones\"
   ```

   and put the headphones in pairing mode near the Core2 (the console's
   `c<name>` changes the name in such a build, saved on the device).
   Release builds have no name.

5. **Host unit tests** for the portable core (needs a host C++ compiler, e.g.
   MinGW-w64): `pio test -e native`.

## Installing and updating

A release installs from the browser with the web installer, or from the
files on its GitHub Release page with esptool
([Releases and the install page](#releases-and-the-install-page)). Either
way the image is the merged one below.

A build makes two ways to install:

- `pio run -e core2 -t upload` writes the pieces: the bootloader, the
  partition table, otadata and the app.
- `.pio/build/core2/firmware.factory.bin` is the same in one file, written
  at **0x0** (what a web installer, M5Burner or
  `esptool write-flash 0x0 firmware.factory.bin` do). It ends far below the
  settings (NVS at 0xC10000), so an update this way **keeps** them: the
  settings, the touch calibration, the paired headphones and the resume
  point. Leave "erase" unticked for updates. Tick it (`esptool erase-flash`
  first) only for the first install over other firmware (M5Stack's demo,
  UIFlow), whose leftovers would sit where the settings and the filesystem
  go.

`pio run -e core2 -t uploadfs` writes `data/` to the 3.8 MB LittleFS
partition (test audio; the player uses it when no SD card is in). It is
separate from the app: an update keeps it, and uploadfs replaces all of it.

The layout ([partitions.csv](partitions.csv),
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md#flash-layout)) is two 6 MB app
slots for later WiFi updates from mStream, then NVS, then LittleFS. It can
never change after release, and `tools/flash_guard.py` fails the build if
the app outgrows its slot (a warning at 80 %), the merged image is missing
or would reach NVS, or the table isn't the one the firmware expects. On
the device, the boot log's `[flash]` line and the console's `L` show the
layout as flashed.

## Versions and releases

Versions are git tags, SemVer with a `v`: `vMAJOR.MINOR.PATCH`. The first
release is **v0.5.0**, a beta (while the major version is 0, anything may
still change). `tools/version.py` names every build from git
(`git describe`) and prints a `version:` line:

| The build | Reads |
|---|---|
| From the tag | `v0.5.0` |
| 3 commits past it, with uncommitted changes | `v0.5.0-3-gabc1234-dirty` |
| No `v*` tag reachable (before the first release, or a clone without its tags: `git fetch --tags`) | `v0.5.0-dev+abc1234` (`-dirty` too) |

The `0.5.0` in the last one is `NEXT_RELEASE`, at the top of
`tools/version.py`: the one place the next version is written. The date a
build shows is its commit's, not the day it was built.

The version shows in **About** (the Version row: the version, under the
commit's date and `ELF` with the first 8 hex digits of the firmware's ELF
SHA-256), in the boot screen's title, in the first serial line
(`mstream-mp3-player v0.5.0 (commit abc1234, 2026-10-01), ELF 1a2b3c4d`), and
in the console's `L`, which shows the image's app description, the version
a later WiFi update will compare. With a crash report, the ELF digits say
which `firmware.elf` decodes its backtrace.

A build with `RELEASE=1` in the environment (CI sets it for a tag) fails
unless HEAD is exactly a SemVer `v*` tag, the tree is clean (untracked
files too: they would be built but aren't in the tag), `BT_SINK_NAME` is
empty, and no `local*.ini` sets build flags. So a release's binary is
exactly the tagged source, the one its licence offer points to. With
`RELEASE_TAG` set too (CI sets it to the tag that started the run), that tag
must be HEAD and is the version, even when the commit has another tag that
`git describe` would pick (an annotated `v0.5.0-rc.1` wins over a
lightweight `v0.5.0`). Releases are built by CI only
([below](#releases-and-the-install-page)); to try a release build locally,
from a clean checkout of a tag (PowerShell):

```powershell
Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue
$env:RELEASE = "1"; $env:RELEASE_TAG = "v0.5.0"; pio run -e core2
```

## Releases and the install page

[`.github/workflows/firmware.yml`](.github/workflows/firmware.yml) runs on
every push, pull request and `v*` tag: `pio test -e native`, then
`pio run -e core2` (its `flash_guard` and `version` checks fail a bad
image), then [`tools/package_release.py`](tools/package_release.py), which
packages the build. The packaged files are kept as the run's artifact
(`mstream-player-core2-<version>`, for 90 days), so any commit's firmware
can be downloaded from its run. A `v*` tag also:

1. builds with `RELEASE=1`;
2. makes a **GitHub Release** with the files below and the notes from
   [`.github/release-notes.md`](.github/release-notes.md), the version
   filled in (hardware, install and update commands, the SD card, drivers,
   known limits, and "Source for this binary: …/tree/<tag>"). A tag with a
   hyphen (`v0.5.0-rc.1`) is marked pre-release. Only the highest
   `vX.Y.Z` tag is marked **Latest**, so a fix tagged on an older line
   doesn't take it. Re-running the workflow finishes a release an
   interrupted run left as a draft (its files replaced, its notes kept,
   then published); a published release is left as it is;
3. puts the **web installer** on GitHub Pages,
   <https://irosthebeggar.github.io/mstream-mp3-player/>: ESP Web Tools'
   install button ([`site/index.html`](site/index.html)), `manifest.json`
   and the firmware, all on the same origin (release files have no CORS
   headers, so the browser can't fetch them from GitHub). Only for the
   newest full release, the highest `vX.Y.Z` tag: not for a pre-release,
   and not for an older tag (pushed together with a newer one, a fix on an
   old line, a re-run, or Run workflow on it), so the installer never goes
   back to older firmware. Pages holds that one version; older ones are on
   the Releases page.

| Release file | What it is |
|---|---|
| `mstream-player-core2-<v>-full.bin` | `firmware.factory.bin`: everything, written at 0x0 (install, or update without erasing) |
| `…-app.bin` | `firmware.bin`, at 0x10000 |
| `…-parts.zip` | bootloader 0x1000, partitions 0x8000, boot_app0 0xe000, app 0x10000, and `flash_args.txt` (`esptool --chip esp32 write-flash @flash_args.txt`) |
| `…-elf.zip` | `firmware.elf` and `firmware.map`, to decode a crash's backtrace (match the ELF digits in About) |
| `…-licenses.zip`, `LICENSE`, `THIRD-PARTY-NOTICES.md` | The licences (`LICENSES/` is in the zip) |
| `…-source.tar.gz` | The source the binary was built from: this repository's files, the git-pinned `lib_deps` checkouts (ESP8266Audio, ESP32-A2DP) and the parts of the Arduino core the build compiled, so the GPL and LGPL source stays available beside the binary even if an upstream download goes away |
| `SHA256SUMS` | `sha256sum -c SHA256SUMS` |

The filesystem image (`littlefs.bin`, `data/music`) is never packaged.

**Cutting a release:**

1. On `main`, on the commit to release: CI is green, and `NEXT_RELEASE` in
   `tools/version.py` is the version (a release build warns when it
   isn't).
2. Tag and push the tag:

   ```
   git tag -a v0.5.0 -m "v0.5.0 (beta)"
   git push origin v0.5.0
   ```

3. Watch the run (Actions > firmware). When it's green, check the release
   page and the installer, and install it once with the installer.
4. Set `NEXT_RELEASE` to the next planned version, so a clone without tags
   doesn't read as the one just released.

**Once, in the repository's settings on GitHub:**

- **Pages > Build and deployment > Source: GitHub Actions.** (Pages needs
  the repository to be public on a free plan.)
- **Environments > github-pages > Deployment branches and tags:** add a
  tag rule `v*`. Pages creates that environment allowing only the default
  branch, and the deploy runs from the tag; without the rule it is refused.
- **Actions > General > Workflow permissions** can stay at the read-only
  default: the workflow asks for what each job needs (`contents: write`
  for the release, `pages: write` and `id-token: write` for the deploy).
  If the organisation caps them lower, the release and deploy jobs fail.

**Locally:** after `pio run -e core2`, `python tools/package_release.py`
writes the same files to `dist/` (`dist/release/`, `dist/site/`,
`dist/release-notes.md`). `--release --tag v0.5.0` packages only a release
build: one made with `RELEASE=1` that passed its checks (a plain build of
the tag, with a `local.ini` say, is refused), from a checkout still clean
and at that commit. To try the page, serve `dist/site` over
`http://localhost` (Web Serial needs HTTPS or localhost).

## Using it

The three touch buttons under the screen do the same on every screen:
**prev** (past a track's first 3 s: back to its start, and paused it
stays paused; within them: the track before) (hold: volume down 5 %, again
every 0.2 s while held),
**play/pause** (hold: switch between speaker and Bluetooth; switching to the
speaker pauses first, so a slow press meant as a pause never moves the music
out loud: play/pause starts it again), **next** (hold: volume up). A hold
acts at half a second. The vibration motor confirms: one tick for a click
(or a tap on the screen that did something), a double tick the moment a hold
is recognised (a button's, or a long press on a list row that has one;
console `ah0` turns that off). A slow press on anything else on the screen
still counts as a tap. Volume is per output: the speaker and
Bluetooth keep their own. A hold shows the volume (or where the output
went) over the tab bar for a moment.

**The screen** has a tab bar at the top, on every page: **Now Playing** (its
icon's bars move while playing, with a progress line under them),
**Library**, **Queue** (a badge with the number of tracks up next), **Dance**,
and **Output**, which also shows the volume and the battery, and whose icon
is cyan with the headphones connected, amber while connecting, red when they
dropped out. Tap a tab to go there: each tab keeps its place (the Library
where you left it, the lists where they were scrolled). Tap the tab you are
on to go back to its start, "‹" in a page's header to go back one step.
Lists scroll with a finger and fling (a touch stops a moving list without
tapping a row). A long A-Z list (over 30) has a letter rail on the right
(hidden while the list moves): slide a finger down it to go through the
list, or tap it for a grid of the letters; a letter with many entries
opens a second grid of its two-letter starts ("Ka", "Ke", "Ki"...). What the
pages do:

- **Now Playing**: the album's cover, the title, the artist and the album
  (tap either, or the cover, to open it in the Library, scrolled to the
  playing track), the progress, the volume (tap: a slider), prev /
  play-pause / next, and "..." for Go to artist, Go to album, Show in
  folders.
- **Library**: three lists at the top: **Artists**, **Albums** (with their
  covers) and **Folders** (the card's folders; only audio files are listed,
  the others counted: "14 audio files, 1 other"). An artist opens its
  albums and "All tracks"; an album or a folder its tracks. Play / Play
  next / + Queue sit at the top of each (at the Folders' top: "Play all N"
  and + Queue), and under a track once you tap it (Play on a track plays its
  album or folder from there); a long press on any row offers the same
  without opening it. What plays is tinted in every list. Every change to
  the queue shows a message at the top with **Undo** (and **View** after an
  add: the Queue at the added tracks). Two levels down, "‹ Library" in the
  header goes straight back to the lists.
- **Queue**: the queue, opened on the playing track, with what's left in
  the header ("4 of 16 · 12 up next · 49 min": lengths are learned as
  tracks play, "49+ min" until they all are). Tap a track for Play now /
  Play next / Remove. After Play next or + Queue in the Library, the next
  visit shows the added tracks, highlighted. **Edit** (or a long press on
  a track) selects: tap tracks, or All; then Remove, Play next, or
  Clear... ("Clear up next" keeps the playing song; "Clear queue" stops the
  music, and asks first). Every change can be undone from the message at
  the top. A track that couldn't be played is skipped with a note, and
  keeps a small "!". Tap the "Queue" title to go to the playing track, the
  top, the end, in turn. Empty, it offers Open Library and Shuffle all.
- **Dance**: the dancer (below).
- **Output**: a card for the Bluetooth headphones (connected, with the
  codec and delay; connecting, "try 2 of 3", with Cancel; failed, with Try
  again; Disconnect; Forget, which takes a second tap within 3 s, or,
  while connected, is in the "..." sheet and asks first) and one for the
  speaker; each shows its own volume (tap it for a slider). Tap a card to
  make it the output: the music stays where it is until the headphones
  are connected ("Now playing on ..." says when), and it pauses whenever
  it leaves them. Forgotten headphones stay forgotten (not even looked
  for by name) until you pair some again. With none paired the card reads
  "No headphones paired": tap it, or its button, for the Pair screen.
  **Pair new headphones** lists the audio devices in pairing mode nearby
  (with their signal); tap one to pair it, in place of the ones paired
  before (they stay if the new pairing fails). Then the line-out module's
  place (not fitted yet), **Haptics** on/off, **Screen off after**,
  **Brightness**, **Turn off when idle**, **CPU speed** (240 MHz, the default,
  is the smooth one; 160 MHz saves a little battery, but lists scroll at
  about half speed while music plays; it takes a restart, asked first: the
  music pauses, and after it waits at the same second until you play) and
  **Bluetooth power** (Low, Normal,
  High), **Touch calibration** ("Not calibrated", or "Calibrated on this
  Core2": below), and **About** (battery, storage, library, headphones,
  CPU speed and Bluetooth power, memory, version, the licence and where
  the source is, and the tips again).

The first time, two tips show what the three red buttons do and that
tapping the tab you're on goes back to its start (console `uic` shows them
again). With no microSD card (and no music on the flash), the pages say so
and offer **Try again** (with a card in, the player restarts to use it);
a card without music offers the same, which looks through `/music` again.
If the headphones drop out while playing, the music pauses (it never
carries on out loud) and a message follows their reconnecting, with **Use
speaker** or **OK** to keep waiting.
The headphones' own buttons work too (play, pause, next, previous, volume),
but never start music that wasn't playing: their play resumes paused
playback (not from stopped), and their next/previous while paused or stopped
only select the track: the Core2's play starts it, or the headphones' play
when paused. Their previous past a track's first 3 s goes back to its start,
as the Core2's does.
Headphones with AVRCP absolute volume (most current ones) take over the
Bluetooth volume when they connect: the Core2 and the headphones show the same
value, and changing it on either side changes both. (Headphones whose remote
control only comes up after playback started take over mid-song: the music
goes silent for about a second, then fades back in over ~2 s, so the
change of their level is never heard as a jump.)

The music is the `.mp3` and `.flac` files under `/music`
(`/music/Artist/Album/NN - Title.mp3`). Any sample rate from 8 to 48 kHz
plays, on the headphones and on the speaker alike (48 kHz files too, over
Bluetooth): the Core2 converts everything that isn't 44.1 kHz to 44.1 kHz
as it decodes ([docs/RESAMPLER.md](docs/RESAMPLER.md)). 88.2 and 96 kHz
files are skipped for now ("96 kHz isn't supported"): they are turned on
once measured on the device, and will then need the 240 MHz CPU speed
(Output > CPU speed). Anything else (176.4/192 kHz, odd rates) is skipped
with a note that names the rate. The files are indexed at boot; the index is cached
in `/.player` on the card and rebuilt when anything under `/music` changes.
An album's cover is the `cover.jpg` in its folder (else `folder.jpg`,
`front.jpg`, or the largest `.jpg` there). It is made into thumbnails the
first time it shows, which are kept in `/.player/thumbs` (delete that folder
to have them made again, after replacing a cover under the same name). A
progressive JPEG can't be decoded on the Core2: its album shows a note
instead.
The first time, the queue is the whole library (artist, album, track order)
followed by three built-in test tones and six click tracks (60 s at 90-174
BPM, for the beat tracker). The queue and its position are saved on the card:
after a restart it's where it was, stopped. A pause also saves the second
it paused at: after the boot that follows (the CPU speed's restart, the
idle power-off, the power key while paused) Now Playing shows that second
and play picks up there; next or another track start from the top, and
previous goes to the top of that track without starting it. (A power cut
while playing starts the track from its beginning:
nothing is written while it plays.) The Library and Queue tabs edit
it, and so do the console's `q` commands (play an album, play it next, add
it, remove, clear, undo).

**Touch calibration.** Core2 touch panels differ: one measured reads
touches on the right half of the screen too far right (about 20 px at x
190, 35-45 px from x 240, and it stops at 319), the same with any finger;
another may read true. Out of the box nothing is corrected. Every button
in the calibration is a full-width bar, which works however far off the
taps land sideways, and the red **A** dot under the screen is the way out
of every step ("A: Cancel" over it says what it does there; tapping that
label on the screen does the same).

- **The touch check.** The first start with nothing calibrated shows three
  dots, one at a time, before the tips: tap the centre of each. Each tap
  leaves a mark where the Core2 read it. If they land off ("Taps land
  about 40 px to the right of your finger. Calibrate now?"), **Calibrate**
  goes on to the crosses and **Not now** leaves it (it's in Output > Touch
  calibration any time); else "Touch is accurate", and a tap goes on. Once
  skipped or answered, it doesn't come back; left alone, it closes after a
  minute and asks again at the next start.
- **Output > Touch calibration** opens a sheet: **Calibrate** (9 crosses,
  about 20 s), **Test taps** (tap anywhere and see where each tap lands),
  and, once calibrated, **Remove calibration** (asked first; taps are then
  read as the panel reports them).
- **Calibrate**: tap the centre of each cross with the finger you usually
  use (a buzz: missed, tap again; a tick and a green flash: taken). Then
  it compares: "Now: up to 42 px off, average 21" against "Calibrated: up
  to 12 px off, average 6", with **Save**, **Try again** and **Discard**.
  The calibrated figures are honest: each tap is measured with a
  calibration made from the other taps, so finger wobble can't flatter
  it (on a panel that is off, a tap it reads at its very edge, where every
  finger further out reads the same, is measured with the calibration
  itself). Save comes first only when the calibration is clearly better
  (3 px or more on average); when the touch is already accurate (or the
  new calibration is no better) it says there's no need to save, and
  Discard comes first. After Save, the Test taps page: tap anywhere, and
  the ring should land under your finger (a grey dot: where it would land
  without the calibration); A undoes the Save there.
- **The rescue.** If the taps are too far off to reach the Output tab,
  switch the Core2 off and on, and once the start-up screen shows (it says
  "Touch trouble? Hold a finger on the screen."), hold a finger anywhere
  on the screen for 2 s: the calibration opens. Put the finger on after
  the screen lights, not while switching on.

On the console: `a` + Enter opens the crosses (`a5` for 5), `ac` the check
page, `ab` the first-start touch check (`ab0` has it ask again at the next
start), `ad` removes the calibration.

**Dancing crab** (proof of concept, [docs/MASCOT-POC.md](docs/MASCOT-POC.md)):
on the Dance tab (or send `d`), a pixel-art crab
dances to what's playing: it lands, squashes and snaps a claw on every beat.
The Core2 finds the beat itself (tempo and phase from the audio, no metadata
needed), over Bluetooth and on the speaker, and the crab idles (breathing,
blinking, glancing around, in dimmed colours) when there's no beat to follow.
A tap on the crab, or `m`, swaps it for the first proof of concept, a stick
figure, and back.

![The crab over two beats, phase 0/8 to 7/8 of each](docs/img/crab-phases.png)

The serial console (115200 baud) is there for scripted testing:

| Key | Action | Key + Enter | Action |
|---|---|---|---|
| `n` / `p` | next / previous (past 3 s: the track's start) | `i<n>` | play queue entry n (0-based) |
| space | play / pause | `b<n>` | benchmark decoding track n (and, at another rate than 44.1 kHz, decode + convert) |
| `o` | switch output | `c<name>` | the name a build with `BT_SINK_NAME` scans for while none are remembered (saved) |
| `+` / `-` | volume | `h<n>` | Bluetooth headroom -n dB, 0-12 (default 2, not saved) |
| `s` / `l` | stats / list the queue | `t<bpm>` | tempo prior for the dance (`t` clears) |
| `f` | forget the paired headphones and restart | `y<ms>` | dance latency offset (not saved) |
| `z` | silent test mode: speaker at volume 0, Bluetooth doesn't take over (until restart) | `k<n>` | freeze the dance pose, 0-15 (`k` unfreezes) |
| `d` / `v` | the Dance tab (again: back) / per-beat log | `ui` (`ui0`-`ui4`, `uib`) | the UI's navigation state: each tab's stack, scroll positions, frames, bus holds, the loop's stack; `ui<n>` taps tab n, `uib` goes back; a scripted finger for tests: `uit<x>,<y>` tap, `uih<x>,<y>` long press, `uis<x0>,<y0>,<x1>,<y1>,<ms>` swipe (a fling when fast), `uid...` drag, `uip<x>,<ms>` a press on the button strip (y >= 240 is the strip in all of them); `uil<n>` the Library shows a made-up library of n tracks (look only, to see the lists at scale), `uil0` the card's again; `uic` the coach cards, `uiT` decode the covers again (timings), `uiV` the volume HUD, `uiF<c/s/p/l/n>` show a faked Bluetooth (connecting, searching, pairing, lost) or no-card state for screenshots, `uiF0` the real one; `uk1` the scripted finger on a skewed panel (the measured one's x), `uk2` the same with up to 4 px of jitter, `uk0` off: the touch check and the calibration run end to end without a hand |
| `m` | next dancer: crab (default) / stick figure | | |
| `x` / `X` | screenshot of the dancer / whole screen (base64 RGB565) | `q...` | the queue: `q` status, `qa` play everything, `qb` the built-in tracks, `ql` list albums, `qp<n>` / `qn<n>` / `q+<n>` album n: play / play next / add, `qr<n>` remove entry n, `qc` clear up next, `qx` clear, `qu` undo, `qs<sec>` start the current entry that far in, as a resume point would (`qs0` none) |
| `L` | the partition table as flashed, the running app slot and the next, NVS use (the boot log has a `[flash]` line too) | `P...` | power measurement ([ARCHITECTURE.md](docs/ARCHITECTURE.md#power-measurement)): `P` a line (5 s of the power chip's readings: USB in, battery, the state), `Pl` one every 5 s, `Pw` to `/.player/power.csv`, `Pm<name>` a marker, `Pq1` the coulomb counter; A/B knobs (`P?`): backlight, screen off, CPU clock, Bluetooth TX power, 5 V boost, LED, IMU, speaker amp, loop delay, the dance tracker, the background reconnect; `Pz` plays an hour of silence |
| | | `R...` | the rate converter ([docs/RESAMPLER.md](docs/RESAMPLER.md)): `R` the current track's conversion (the exact ratio, source frames taken, ring frames made, clamped samples); `Rt` lists its test tracks (a 1 kHz tone and silence at other rates), `Rt<n>` or `Rt<tone:...@rate>` plays one on its own (the player is stopped first, keeping your place in the track: nothing follows it; only silence on Bluetooth, a tone only in silent mode `z`); `Rf</music/...>` plays a file on its own (silent mode only; `Rf48000</music/...>` converts it as if it were 48 kHz, a load test), `Rx` stops what `Rt` or `Rf` started; `Rb` its bench (the MAC16 kernel's self-test and route check, then 10 s of audio per rate with each kernel: cycles and share of a core at the clock running; 88.2/96 kHz too, though they don't play yet) |
| | | `B...` | Bluetooth tests that leave your pairing alone: `B` status; `Bs` auto-pair by signal for the next scan (a device at -55 dBm or closer, whatever its name; RAM only, off at boot, logged; it starts that scan, with none remembered: `Bn` first), `Bs0` off; `Bf` the next boot as a fresh unit (a flag that boot clears: as if nothing were remembered and there were no `BT_SINK_NAME`, the stored address and the bond not read or touched; restarts now); `Bn` the same for this session (RAM only; not while linked or pairing), `Bn0` back |
| | | `a...` | touch and haptics: `a` touch calibration (9 crosses; `a5`-`a9` for fewer), `ac` test taps, `ab` the first-start touch check (`ab0`: ask it again at the next start), `as` status, `ad` remove the calibration (no correction), `ah0` / `ah1` haptics off / on, `ar0` / `ar1` the A-Z rail's ticks off / on, `aq` close (saved on the device) |

The UI spike's tools ([docs/UI-SPIKE.md](docs/UI-SPIKE.md)) measure the
browsing UI's risks before its screens are built. Each is a letter, an
optional argument and Enter:

| Command | Tool |
|---|---|
| `u` (`u0`-`u3`, `us`, `uh<ms>`, `ut<ms>`) | input lab: button and glass-touch logging, tab target practice, button practice, haptic ticks, percentile summary |
| `w` (`w0`-`w3`, `wv` `wf` `wh` `wc` `wd` `wk` `wg` `wm` `wp` `ws`) | scroll lab: a flick-scrolled library list, interactive or a 60 s stress while audio plays (hardware scroll and 30 fps by default) |
| `g` (`g0`, `g<n>`) | library index: report, rebuild from the card (the queue follows by path), or a synthetic library of n tracks for the labs (the player keeps the real one) |
| `e` (`e1`-`e5`) | font probe: list rows in FreeSans (folded) and the UI's VLW font, timed (efont only in a build with `-DUI_SPIKE_EFONT=1`) |
| `j` (`j<n>`, `jw<n>`, `ja`) | thumbnail probe: cover.jpg to 40x40 / 80x80, decoder RAM, PSRAM and .565 caches |

## Layout

```
platformio.ini        Build envs: core2 | native; local*.ini holds per-developer overrides
partitions.csv        Two 6 MB OTA app slots, NVS above anything a single-file
                      install writes, 3.8 MB LittleFS (test audio), coredump;
                      never changes after release (docs/ARCHITECTURE.md#flash-layout)
lib/core/             Portable logic, framework-agnostic (also compiled for native)
  PlaybackController  Transport over the queue; skips tracks that fail
  QueueModel          The play queue: track ids in PSRAM, current position,
                      stable keys, one level of undo
  QueueText           The queue saved as paths (survives a library rebuild)
  TrackCatalog        Track ids to paths and names: the index's tracks and
                      the built-in ones
  QueueSaver          When the queue, its position and the resume point
                      (the second a paused track picks up at) are saved
  TrackProgress, TrackSeek
                      A track's length (headers, read rate); starting part
                      of the way in (an MP3's byte from its bitrate or TOC,
                      a clean frame, a FLAC's STREAMINFO)
  ByteStream          Byte sinks and sources for what is saved and loaded
  HeadsetKeys         What the headphones' transport keys do (never start music)
  PcmRing             PCM ring between the decode task and the active output
  RateConverter, ResamplerTables
                      Any supported rate to 44.1 kHz: Q15 polyphase and
                      halfband FIRs, exact counts (docs/RESAMPLER.md)
  RingFeed            The decode side of the ring: the converter, the stage,
                      the ring-full rule (RingOutput wraps it)
  TransportSync       Generation-tagged decode progress (no stale "track ended")
  ToneGen, ClickGen   Built-in test tones; click tracks with a known beat
  ToneTrack           What a built-in track's path asks for ("tone:1000@48000")
  AudioTap, TapReader What an output played, placed in its track; audible-time clock
  BeatTracker         Tempo, phase and confidence from the audio (onsets, ACF, PLL)
  DancePose           The stick figure's joints as functions of the beat phase
  CrabPose, CrabArt   The crab's layer frames and offsets per beat phase; its
                      pixel art (generated from tools/art/crab.json)
  DanceSkin           Which dancer is on screen (crab, stick)
  DanceRate           The Dance tab's frame rate: 10 fps idle, 24-30 dancing
  LibraryIndex        The library in a few PSRAM blocks: string arena, records,
                      sorted views, A-Z buckets, folder tree; saved and loaded
                      as one file (LibrarySynth: made-up libraries of any size)
  TextFold            UTF-8 to ASCII for the GFX fonts; the library's sort order
  JumpIndex           The A-Z jump grid: each letter's first row, and a big
                      letter's two-letter starts
  ThumbCache, ThumbScaler, JpegInfo
                      Album covers as thumbnails: the PSRAM LRU and what's
                      asked, the card's .565 files; the box-filter scaler;
                      a JPEG's size and whether it is progressive
  TouchCalibration    The touch correction: a monotonic piecewise-linear table
                      per axis, its fit to taps, and its bytes for NVS
  TouchCheck          The first-start touch check's verdict and when it
                      shows; the calibration's crosses, misses and result
  TouchRecognizer, StripButtons, ButtonGesture, ButtonPolicy, InputEvent
                      The glass's tap/hold/drag/fling events; the touch
                      buttons (only a touch that went down on one, and
                      stays), their click/hold/repeat, and what they do
  PowerWindow         The power chip's (AXP192) ADC registers decoded, and
                      their mean/min/max over a window (console P)
  OutputModel         The Output tab's Bluetooth card (its state for every
                      link state, a connection that failed), what the
                      listener asked for (BtSession), Forget's second
                      tap, the pairing scan's list
  UiText              The UI's fixed texts next to their room (the host
                      tests measure them with the firmware's fonts)
  QueueView           The Queue's summary from learned track lengths, the
                      mark on what a Library add put in, the failed-track
                      ring, Shuffle all
  TouchGesture, KineticScroll, ScrollGovernor
                      Tap/hold/drag/flick; inertial list scrolling (flings
                      capped at 2,000 px/s); how hard a list may use the SPI
                      bus, from the audio buffer's fill
  VScrollMap          The LCD's hardware vertical scroll: which lines to draw
  RefillPacer         The decoder's gentle refill after a track start (on)
  BtControl           Bluetooth decisions: media stream (StreamControl), volume
                      (AbsVolumePolicy, GainRamp), reconnect (ReconnectPlanner)
  SinkSearch          When the Core2 may scan for headphones by itself (never
                      in a release build) and which find it may take (by the
                      build's name only; never by signal but for console Bs)
  hal/                IAudioBackend, IStorage
src/                  Core2 firmware
  audio/              Core2AudioBackend (decode task), RingOutput, BtSink, SpeakerSink
  storage/            LocalStorage: SD card if present, else LittleFS; FileStream
  ui/                 Ui (the one owner of the display: navigation, tab bar,
                      overlays, pages), TabBar, ListView (lists on the
                      hardware scroll), Overlays (toast, HUD, sheet, volume
                      sheet, jump grid, dialog, coach cards), EmptyState,
                      the pages (NowPlaying, Library, Queue, Dance, Output
                      with Pair and About), Thumbs (album covers:
                      a worker task decodes them below the loop),
                      Fonts + VlwFonts (DejaVu, anti-aliased), Icons +
                      IconData, Gfx (pushes through the scroll and the
                      bus lock), Theme; Input (the one input layer: corrected
                      touches and the buttons as events, haptic feedback,
                      settings in NVS); CalibrationScreen (touch calibration);
                      BootScreen (the boot diagnostics); DanceView;
                      ListScroller (hardware scroll); LcdLock (times how long
                      the LCD holds the SPI bus)
  app/                Library (the index at boot: cache or build), QueueStore
                      (the queue on the card, its position in NVS), Psram,
                      SerialConsole, Diagnostics, DanceMode, Screenshot, Haptics,
                      PowerProbe + PowerLab (power measurement and its knobs),
                      Version (the build's version and ELF hash; the image's
                      app description)
  spike/              UI spike tools: input lab, scroll lab, font and thumbnail
                      probes (docs/UI-SPIKE.md)
  main.cpp            Wires it together; input events (the buttons' policy),
                      Bluetooth events, what the UI reads (UiHost), the
                      console's queue and touch commands
data/                 LittleFS image source (data/music is gitignored)
tools/                make_test_audio.py; version.py (build pre-script: the
                      version from git, the release checks); iram_diet.py and
                      flash_guard.py (build post-scripts: IRAM; the app's slot,
                      the merged image, NVS, the pieces' list);
                      package_release.py (a build's release files, install
                      page and release notes -> dist/);
                      crab_art.py + art/crab.json (the crab's art -> lib/core/CrabArt.*);
                      vlw_font.py (the UI's DejaVu VLW fonts -> src/ui/VlwFonts.cpp);
                      ui_icons.py (the UI's 1-bit icons -> src/ui/IconData.cpp);
                      gen_resampler_tables.py (the rate converter's filters ->
                      lib/core/ResamplerTables.cpp)
test/                 Host unit tests (Unity)
site/                 The web installer's page (filled in by package_release.py)
.github/              workflows/firmware.yml (CI, releases, the install page);
                      release-notes.md (the release notes' template)
docker/               mStream dev server
docs/                 ARCHITECTURE.md, POC-RESULTS.md, MASCOT-POC.md, UI-SPIKE.md,
                      RESAMPLER.md
LICENSES/             The licence texts THIRD-PARTY-NOTICES.md refers to
THIRD-PARTY-NOTICES.md What else is in the firmware binary, and its licences
```

## License

Copyright (C) 2026 IrosTheBeggar.

This program is free software: you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the Free
Software Foundation, either version 3 of the License, or (at your option)
any later version (GPL-3.0-or-later; the text is [LICENSE](LICENSE)). It is
distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.
Every source file says so in its first lines (`SPDX-License-Identifier`).

The firmware binary also contains other people's code, fonts and binary
libraries under their own licences, not all of them the GPL:
ESP8266Audio (GPL-3.0-or-later) with libmad (GPL-2.0-or-later) and libFLAC
(BSD-3-Clause), ESP32-A2DP (Apache-2.0), M5Unified and M5GFX (MIT, with
LovyanGFX and fonts under BSD-style licences), the Arduino-ESP32 core
(LGPL-2.1-or-later and Apache-2.0), ESP-IDF with Espressif's binary
Bluetooth and radio libraries (Apache-2.0, with BSD- and MIT-licensed parts
such as TinyCrypt, TLSF and littlefs), newlib and the GCC runtime, and
the DejaVu fonts the UI is drawn in (Bitstream Vera licence). Each is listed
with its version, copyright and licence in
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md); the licence texts are in
[LICENSES/](LICENSES). The Core2 shows the licence in Output > About and
prints it on the serial console at boot. Each release carries its source
as `…-source.tar.gz`, the GPL and LGPL libraries it was built with
included.

M5Stack is a trademark of M5Stack Technology Co., Ltd.; Bluetooth
is a registered trademark of Bluetooth SIG, Inc.; Beats and Powerbeats are
trademarks of Apple Inc. They are named only to say what this runs on and
what it was tested with. This project is not affiliated with, sponsored or
endorsed by any of them, and makes no Bluetooth qualification claim.
