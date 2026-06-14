#pragma once
#include <cstdint>

// Reads the rotary encoder + the Play/Prev/Next buttons and the encoder push
// switch, with debouncing. poll() returns a snapshot of edges since last call.
struct InputEvents {
  int encoderDelta = 0;   // net detents this poll: +clockwise / -counterclockwise
  bool select = false;    // encoder push switch pressed
  bool play = false;
  bool prev = false;
  bool next = false;

  bool any() const { return encoderDelta || select || play || prev || next; }
};

class Controls {
public:
  void begin();
  InputEvents poll(uint32_t nowMs);

private:
  // Encoder quadrature state
  int lastClk_ = 1;

  // Debounced buttons (active-low). Returns true once per press.
  struct Button {
    uint8_t pin = 0;
    int lastRaw = 1;
    int stable = 1;
    uint32_t lastChangeMs = 0;
    Button() = default;
    explicit Button(uint8_t p) : pin(p) {}
    bool pressedEdge(uint32_t nowMs);
  };
  Button btnSelect_;
  Button btnPlay_;
  Button btnPrev_;
  Button btnNext_;
};
