// mstream-mp3-player — firmware entry point.
//
// Wires the portable core (PlaybackController, DockController) to the hardware
// HAL implementations and runs the UI loop. Everything hardware-specific is
// behind a HAL interface, so the core stays unit-testable on the host.

#include <Arduino.h>
#include <utility>  // std::move
#include <vector>

#include "PlaybackController.h"
#include "DockController.h"
#include "Track.h"

#include "audio/SimAudioBackend.h"
#include "storage/SdStorage.h"
#include "dock/SimDock.h"
#include "input/Controls.h"
#include "ui/DisplayView.h"

// ---- HAL + core instances ----
static SimAudioBackend audio;
static SdStorage storage;
static SimDock dock;
static Controls controls;
static DisplayView view;

static PlaybackController player(audio);
static DockController dockCtrl(dock);

// ---- UI state ----
enum class Screen { Library, NowPlaying };
static Screen screen = Screen::Library;
static int selected = 0;
static int topRow = 0;
static bool dirty = true;            // needs a redraw
static uint32_t lastNowPlayingDraw = 0;

// A built-in library so the sim shows content even with no SD files loaded.
static std::vector<Track> demoLibrary() {
  return {
    {"/demo/midnight_drive.mp3", "Midnight Drive", "Neon Cassette", 215000},
    {"/demo/paper_planes.mp3", "Paper Planes", "The Slow Hours", 188000},
    {"/demo/glass_oceans.mp3", "Glass Oceans", "Marlowe", 242000},
    {"/demo/no_signal.mp3", "No Signal", "Held Static", 167000},
    {"/demo/afterglow.mp3", "Afterglow", "June & The Tide", 203000},
    {"/demo/lowlight.mp3", "Lowlight", "Cabinet", 198000},
    {"/demo/dust.mp3", "Dust", "Ferrous", 221000},
    {"/demo/citrus.mp3", "Citrus", "Pale Green Things", 175000},
    {"/demo/undertow.mp3", "Undertow", "Marlowe", 256000},
    {"/demo/static_bloom.mp3", "Static Bloom", "Held Static", 184000},
  };
}

static void loadLibrary() {
  std::vector<Track> tracks;
  if (storage.available()) tracks = storage.listTracks();
  if (tracks.empty()) {
    Serial.println("[lib] using built-in demo library");
    tracks = demoLibrary();
  } else {
    Serial.printf("[lib] loaded %u tracks from SD\n", (unsigned)tracks.size());
  }
  player.setPlaylist(std::move(tracks));
  selected = 0;
  topRow = 0;
}

static void clampScroll() {
  const int n = static_cast<int>(player.playlist().size());
  if (selected < 0) selected = 0;
  if (selected >= n) selected = n - 1;
  const int rows = view.visibleRows();
  if (selected < topRow) topRow = selected;
  if (selected >= topRow + rows) topRow = selected - rows + 1;
  if (topRow < 0) topRow = 0;
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\nmstream-mp3-player booting...");

  view.begin();
  controls.begin();
  dockCtrl.begin();
  storage.begin();
  loadLibrary();

  // Dock handoff side effects (the core stays storage/audio-agnostic).
  dockCtrl.onDock = []() {
    player.stop();
    storage.releaseToHost();
    screen = Screen::Library;
    dirty = true;
  };
  dockCtrl.onUndock = []() {
    storage.reclaim();
    loadLibrary();
    dirty = true;
  };
}

void loop() {
  const uint32_t now = millis();

  dockCtrl.update(now);
  audio.loop(now);
  player.update(now);

  // While docked, the card belongs to the host — show the docked screen and
  // ignore everything except the dock toggle (handled in dockCtrl.update).
  if (dockCtrl.state() == DockState::Docked) {
    if (dirty) {
      view.showDocked();
      dirty = false;
    }
    delay(5);
    return;
  }

  InputEvents ev = controls.poll(now);

  if (screen == Screen::Library) {
    if (ev.encoderDelta) {
      selected += ev.encoderDelta;
      clampScroll();
      dirty = true;
    }
    if (ev.select || ev.play) {
      player.play(static_cast<size_t>(selected));
      screen = Screen::NowPlaying;
      dirty = true;
    }
    if (ev.next) { player.next(); screen = Screen::NowPlaying; dirty = true; }
    if (ev.prev) { player.prev(); screen = Screen::NowPlaying; dirty = true; }
  } else {  // NowPlaying
    if (ev.play) { player.togglePlayPause(); dirty = true; }
    if (ev.next) { player.next(); dirty = true; }
    if (ev.prev) { player.prev(); dirty = true; }
    if (ev.select) { screen = Screen::Library; dirty = true; }
  }

  // Render. NowPlaying also refreshes a few times a second for the progress bar.
  if (screen == Screen::Library) {
    if (dirty) {
      view.showLibrary(player.playlist(), selected, topRow);
      dirty = false;
    }
  } else {
    if (dirty || (now - lastNowPlayingDraw) >= 250) {
      const Track* t = player.currentTrack();
      view.showNowPlaying(t, player.state(), player.positionMs(),
                          t ? t->durationMs : 0);
      lastNowPlayingDraw = now;
      dirty = false;
    }
  }

  delay(5);
}
