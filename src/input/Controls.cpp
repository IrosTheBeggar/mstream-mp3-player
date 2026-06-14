#include "input/Controls.h"

#include <Arduino.h>

#include "Pins.h"

namespace {
constexpr uint32_t kDebounceMs = 25;
}

bool Controls::Button::pressedEdge(uint32_t nowMs) {
  const int raw = digitalRead(pin);
  if (raw != lastRaw) {
    lastRaw = raw;
    lastChangeMs = nowMs;
  }
  if ((nowMs - lastChangeMs) >= kDebounceMs && raw != stable) {
    stable = raw;
    if (stable == LOW) return true;  // active-low press
  }
  return false;
}

void Controls::begin() {
  pinMode(PIN_ENC_CLK, INPUT_PULLUP);
  pinMode(PIN_ENC_DT, INPUT_PULLUP);
  lastClk_ = digitalRead(PIN_ENC_CLK);

  btnSelect_ = Button(PIN_ENC_SW);
  btnPlay_ = Button(PIN_BTN_PLAY);
  btnPrev_ = Button(PIN_BTN_PREV);
  btnNext_ = Button(PIN_BTN_NEXT);
  for (uint8_t pin : {PIN_ENC_SW, PIN_BTN_PLAY, PIN_BTN_PREV, PIN_BTN_NEXT}) {
    pinMode(pin, INPUT_PULLUP);
  }
}

InputEvents Controls::poll(uint32_t nowMs) {
  InputEvents ev;

  // Rotary encoder: read on the falling edge of CLK; DT sets direction.
  const int clk = digitalRead(PIN_ENC_CLK);
  if (clk != lastClk_ && clk == LOW) {
    ev.encoderDelta += (digitalRead(PIN_ENC_DT) != clk) ? 1 : -1;
  }
  lastClk_ = clk;

  ev.select = btnSelect_.pressedEdge(nowMs);
  ev.play = btnPlay_.pressedEdge(nowMs);
  ev.prev = btnPrev_.pressedEdge(nowMs);
  ev.next = btnNext_.pressedEdge(nowMs);
  return ev;
}
