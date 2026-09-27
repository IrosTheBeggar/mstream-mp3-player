#pragma once
#include <atomic>
#include <cstdint>

// State the decode task shares with the two outputs (the Bluetooth callback
// and the speaker pump). All three run on different tasks.
struct AudioShared {
  // Sample rate of the frames in the ring. Set before the first frame of a
  // track is written, so an output that has read a frame sees its rate.
  std::atomic<int> rate{44100};
  // Outputs fade the next 64 frames out, then play silence without reading:
  // pausing takes ~1.5 ms and the ring keeps its audio for resume.
  std::atomic<bool> paused{false};
  // A track is producing and past its pre-roll, so an empty ring now means an
  // audible gap: the outputs count it as an underrun.
  std::atomic<bool> expectingAudio{false};
  std::atomic<uint32_t> underruns{0};
  // The outputs copy what they play to their AudioTap (the beat tracker's
  // input). A power measurement can turn it off (the console's Pk0).
  std::atomic<bool> tapOn{true};
};
