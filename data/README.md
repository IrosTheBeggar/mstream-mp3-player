# LittleFS image

Everything in this folder is written to the Core2's internal-flash filesystem
(3.8 MB, the `spiffs` partition in `partitions.csv`) by:

```powershell
pio run -e core2 -t uploadfs
```

The player uses it as its library when no SD card is inserted: tracks are the
`.mp3`/`.flac` files under `music/`, played in path order.

`music/` is gitignored. Fill it with `tools/make_test_audio.py`, which makes
known-pitch test tones (44.1 kHz, 48 kHz, 24-bit/96 kHz) and optional short
excerpts of your own tracks — see the script's docstring.
