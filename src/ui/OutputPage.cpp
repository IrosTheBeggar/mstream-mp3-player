// Output (a first page for the framework; the spec's §6.6 is the full
// screen): where the music plays, and its volume, and the way into the
// touch calibration. Content y 36-239:
//
//   36-71    header "Output"
//   74-125   the Bluetooth card: its state in its colour (cyan connected,
//            amber connecting, red lost); tap: the output
//   128-167  the speaker card; tap: the output (the music pauses first, as
//            with the B hold: it never moves out loud by itself)
//   170-205  [ − ]  Volume 60% (Bluetooth)  [ + ]   (+ reaches the edge)
//   208-239  Touch calibration ›  |  Forget pairing (a dialog confirms)
#include <cstdio>
#include <cstring>

#include "ui/Fonts.h"
#include "ui/Gfx.h"
#include "ui/Icons.h"
#include "ui/Pages.h"
#include "ui/Theme.h"
#include "ui/Ui.h"

namespace ui {

namespace {
constexpr int kBtY = 74, kBtH = 52;
constexpr int kSpkY = 128, kSpkH = 40;
constexpr int kVolY = 170, kVolH = 36;
constexpr int kLinkY = 208, kLinkH = 32;
constexpr int kVolStep = 5;

void radio(M5Canvas& c, int cx, int cy, bool on, uint16_t acc, uint16_t bg) {
  c.fillCircle(cx, cy, 10, bg);
  c.drawCircle(cx, cy, 10, on ? acc : col::FAINT);
  c.drawCircle(cx, cy, 9, on ? acc : col::FAINT);
  if (on) c.fillCircle(cx, cy, 5, acc);
}
}  // namespace

uint32_t OutputPage::signature() const {
  const AppState& s = ui_.state();
  uint32_t h = (s.onBluetooth ? 1u : 0u) | (s.btConnected ? 2u : 0u) | (s.btLost ? 4u : 0u) |
               (s.headphonesSetVolume ? 8u : 0u) | (s.silent ? 16u : 0u);
  h = h * 31u + s.speakerVolume;
  h = h * 31u + s.btVolume;
  for (const char* p = s.btName; *p; ++p) h = h * 31u + static_cast<unsigned char>(*p);
  return h * 31u + static_cast<uint32_t>(pressed_ + 1);
}

void OutputPage::enter(NavModel::PageRef& ref) {
  (void)ref;
  pressed_ = None;
  repaint();
}

void OutputPage::repaintHeader() {
  Header h;
  h.title = "Output";
  h.sub = "where the music plays";
  ui_.drawHeader(h);
}

void OutputPage::repaint() {
  repaintHeader();
  // The gaps between the rows (each row pushes its own band).
  gfx::fill(0, kBtY - 2, kW, 2, col::BG);
  gfx::fill(0, kBtY + kBtH, kW, kSpkY - kBtY - kBtH, col::BG);
  gfx::fill(0, kSpkY + kSpkH, kW, kVolY - kSpkY - kSpkH, col::BG);
  gfx::fill(0, kVolY + kVolH, kW, kLinkY - kVolY - kVolH, col::BG);
  drawCards();
  drawVolume();
  drawLinks();
  drawnSig_ = signature();
}

void OutputPage::drawCards() {
  const AppState& s = ui_.state();
  Fonts& f = Fonts::instance();
  M5Canvas& c = gfx::strip();
  const uint16_t acc = accent::Output;
  // Bluetooth.
  {
    const bool active = s.onBluetooth;
    const uint16_t bg = pressed_ == Bluetooth ? col::BTN_HI : active ? col::ROW_SEL : col::CARD;
    c.fillRect(0, 0, kW, kBtH, col::BG);
    c.fillRoundRect(6, 0, kW - 12, kBtH - 2, 10, bg);
    if (active) c.drawRoundRect(6, 0, kW - 12, kBtH - 2, 10, acc);
    const uint16_t sc = s.btConnected ? col::CYAN : s.btLost ? col::RED : active ? col::AMBER : col::DIM;
    icons::drawCentred(c, icons::kHeadphones, 30, kBtH / 2 - 1, sc);
    f.draw(c, Font::Bold, s.btName[0] ? s.btName : "Bluetooth headphones", 52, 15, 220, col::TXT, bg);
    char line[64];
    if (s.btConnected) {
      snprintf(line, sizeof(line), "Connected, volume %u%%%s", static_cast<unsigned>(s.btVolume),
               s.headphonesSetVolume ? " (theirs)" : "");
    } else if (s.btLost) {
      snprintf(line, sizeof(line), "Lost: waiting for them to come back");
    } else if (active) {
      snprintf(line, sizeof(line), "Connecting...");
    } else {
      snprintf(line, sizeof(line), "Not the output: tap to use them");
    }
    f.draw(c, Font::Small, line, 52, 34, 220, sc, bg);
    radio(c, 290, kBtH / 2 - 1, active, acc, bg);
    gfx::push(c, 0, kBtY, kW, kBtH);
  }
  // The speaker.
  {
    const bool active = !s.onBluetooth;
    const uint16_t bg = pressed_ == Speaker ? col::BTN_HI : active ? col::ROW_SEL : col::CARD;
    c.fillRect(0, 0, kW, kSpkH, col::BG);
    c.fillRoundRect(6, 0, kW - 12, kSpkH - 2, 10, bg);
    if (active) c.drawRoundRect(6, 0, kW - 12, kSpkH - 2, 10, acc);
    icons::drawCentred(c, icons::kSpeaker, 30, kSpkH / 2 - 1, active ? col::SOFT : col::DIM);
    f.draw(c, Font::Bold, "Speaker", 52, 11, 100, col::TXT, bg);
    char line[48];
    snprintf(line, sizeof(line), "Built-in, volume %u%%%s", static_cast<unsigned>(s.speakerVolume),
             s.silent ? " (silent)" : "");
    f.draw(c, Font::Small, line, 52, 28, 220, col::DIM, bg);
    radio(c, 290, kSpkH / 2 - 1, active, acc, bg);
    gfx::push(c, 0, kSpkY, kW, kSpkH);
  }
}

void OutputPage::drawVolume() {
  const AppState& s = ui_.state();
  Fonts& f = Fonts::instance();
  M5Canvas& c = gfx::strip();
  c.fillRect(0, 0, kW, kVolH, col::BG);
  const uint16_t down = pressed_ == VolDown ? col::BTN_HI : col::BTN;
  const uint16_t up = pressed_ == VolUp ? col::BTN_HI : col::BTN;
  c.fillRoundRect(6, 2, 68, 32, 8, down);
  icons::drawCentred(c, icons::kMinus, 40, 18, col::TXT);
  c.fillRoundRect(kW - 74, 2, 68, 32, 8, up);
  icons::drawCentred(c, icons::kPlus, kW - 40, 18, col::TXT);
  char v[32];
  snprintf(v, sizeof(v), "Volume %u%%", static_cast<unsigned>(s.volume));
  f.draw(c, Font::Bold, v, kW / 2, 11, 150, col::TXT, col::BG, Fonts::Align::Centre);
  f.draw(c, Font::Small, s.onBluetooth ? "headphones" : "speaker", kW / 2, 28, 150, col::DIM, col::BG,
         Fonts::Align::Centre);
  gfx::push(c, 0, kVolY, kW, kVolH);
  drawnVolume_ = s.volume;
}

void OutputPage::drawLinks() {
  Fonts& f = Fonts::instance();
  M5Canvas& c = gfx::strip();
  c.fillRect(0, 0, kW, kLinkH, col::BG);
  const uint16_t a = pressed_ == Calibrate ? col::ROW_SEL : col::BG;
  const uint16_t b = pressed_ == Forget ? col::ROW_SEL : col::BG;
  c.fillRect(0, 0, 178, kLinkH, a);
  c.fillRect(182, 0, kW - 182, kLinkH, b);
  c.drawFastVLine(180, 6, kLinkH - 12, col::DIV);
  f.draw(c, Font::Body, "Touch calibration", 12, kLinkH / 2, 140, accent::Output, a);
  icons::drawCentred(c, icons::kChevronRight, 168, kLinkH / 2, col::FAINT);
  // "Forget headphones" (155 px) doesn't fit the 138 px here.
  f.draw(c, Font::Body, "Forget pairing", 192, kLinkH / 2, 124, col::RED, b);
  gfx::push(c, 0, kLinkY, kW, kLinkH);
}

bool OutputPage::update(uint32_t nowMs, bool frameDue, bool wholeRows) {
  (void)nowMs;
  (void)frameDue;
  (void)wholeRows;
  const uint32_t sig = signature();
  if (sig != drawnSig_) {
    drawCards();
    drawnSig_ = sig;
  }
  if (ui_.state().volume != drawnVolume_) drawVolume();
  return false;
}

OutputPage::Zone OutputPage::zoneAt(const InputEvent& e) const {
  if (e.y >= kBtY && e.y < kBtY + kBtH) return Bluetooth;
  if (e.y >= kSpkY && e.y < kSpkY + kSpkH) return Speaker;
  if (e.y >= kVolY && e.y < kVolY + kVolH) {
    if (e.x < 80) return VolDown;
    if (e.inRightEdgeZone(kW - 80)) return VolUp;  // to the screen's edge
    return None;
  }
  if (e.y >= kLinkY) return e.inRightEdgeZone(180) ? Forget : Calibrate;
  return None;
}

void OutputPage::onEvent(const InputEvent& e) {
  using T = InputEvent::Type;
  if (e.type == T::Down) {
    pressed_ = zoneAt(e);
    if (pressed_ == Bluetooth || pressed_ == Speaker) drawCards();
    if (pressed_ == VolDown || pressed_ == VolUp) drawVolume();
    if (pressed_ == Calibrate || pressed_ == Forget) drawLinks();
    return;
  }
  if (e.type != T::Tap && e.type != T::DragStart && e.type != T::Release && e.type != T::Cancel) return;
  const Zone was = pressed_;
  pressed_ = None;
  if (was == Bluetooth || was == Speaker) drawCards();
  if (was == VolDown || was == VolUp) drawVolume();
  if (was == Calibrate || was == Forget) drawLinks();
  drawnSig_ = signature();
  if (e.type != T::Tap || was == None) return;
  ui_.tick();
  switch (was) {
    case Bluetooth: ui_.host().selectOutput(true); break;
    case Speaker: ui_.host().selectOutput(false); break;
    case VolDown: ui_.host().stepVolume(-kVolStep); break;
    case VolUp: ui_.host().stepVolume(+kVolStep); break;
    case Calibrate: ui_.host().openCalibration(); break;
    case Forget: {
      static const char* const kButtons[2] = {"Cancel", "Forget"};
      char body[128];
      const AppState& s = ui_.state();
      snprintf(body, sizeof(body), "The Core2 forgets %s and restarts to look for headphones in pairing mode.",
               s.btName[0] ? s.btName : "the paired headphones");
      ui_.openDialog(this, "Forget the headphones?", body, kButtons, 2);
      break;
    }
    default: break;
  }
}

void OutputPage::onDialog(int button) {
  if (button == 1) ui_.host().forgetHeadphones();
}

void OutputPage::describe(char* buf, size_t size) const {
  const AppState& s = ui_.state();
  snprintf(buf, size, "Output: %s%s, volume %u%% (speaker %u%%, bluetooth %u%%)", s.onBluetooth ? "bluetooth" : "speaker",
           s.onBluetooth ? (s.btConnected ? " connected" : " not connected") : "", static_cast<unsigned>(s.volume),
           static_cast<unsigned>(s.speakerVolume), static_cast<unsigned>(s.btVolume));
}

}  // namespace ui
