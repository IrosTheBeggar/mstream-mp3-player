// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "DanceFrame.h"

namespace dance {

namespace {
bool isK(int8_t n) { return n >= 0 && n <= 15; }
}  // namespace

BeatInput beatInput(const FrameFacts& f, TempoFold& fold) {
  BeatInput in{};
  in.heard = f.heard;
  in.grid = f.grid;
  in.locked = f.locked;
  in.confidence = f.confidence;
  in.trackerBeats = f.trackerBeats;
  in.rawBpm = f.rawBpm;
  in.foldScale = f.grid ? fold.apply(f.rawBpm) : fold.scale();
  in.restarts = f.restarts;
  in.acquisitions = f.acquisitions;
  in.dt = f.dt;
  in.freeze = isK(f.freeze) ? f.freeze : static_cast<int8_t>(-1);
  return in;
}

dancerate::Scene sceneOf(const FrameFacts& f, const Cue& c) {
  dancerate::Scene s;
  s.frozen = isK(f.freeze);
  s.beat = f.grid;
  s.locked = f.locked;
  s.weight = c.weight;
  return s;
}

}  // namespace dance
