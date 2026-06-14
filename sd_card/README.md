# Simulated SD card contents

Drop `.mp3` / `.flac` files here to give the firmware a real library in Wokwi.

**How Wokwi loads them:** in the Wokwi VS Code extension, files placed next to
`diagram.json` can be attached to the `wokwi-microsd-card` part. (On wokwi.com,
use the SD card part's right-click *"Upload file"*.)

If the card is empty or unreadable, the firmware falls back to a built-in demo
library (`demoLibrary()` in `src/main.cpp`) so the UI always has something to
show — you don't need any files here to start.

Note: the simulated playback head uses each track's duration. SD files report an
unknown duration (0) until real metadata/decoding lands, so their progress bar
won't move in the sim yet; the demo tracks have durations and will.
