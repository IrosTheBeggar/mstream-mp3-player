#include "spike/InputLab.h"

#include <M5Unified.h>
#include <esp_random.h>

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <cstring>

#include "Percentiles.h"
#include "spike/SpikeUi.h"

using namespace spike;

namespace {

constexpr uint32_t kButtonWindowMs = 300;  // a button event this close to a tap counts as false
constexpr uint32_t kFeedbackMs = 600;      // the hit/miss outline shows this long
constexpr int kGlassBottomY = 220;
constexpr int kLcdH = 240;
const char kBtnName[3] = {'A', 'B', 'C'};

// The tabs design's targets (spec §3.1, §6.4, §6.2 toast).
struct TargetDef {
  const char* name;
  int16_t x0, y0, x1, y1;
  uint8_t zone;
  int8_t scene;
};
constexpr TargetDef kTargets[] = {
    {"Now Playing tab", 0, 0, 55, 35, 0, -1},
    {"Library tab", 56, 0, 111, 35, 0, -1},
    {"Queue tab", 112, 0, 167, 35, 0, -1},
    {"Dance tab", 168, 0, 223, 35, 0, -1},
    {"Output tab", 224, 0, 279, 35, 0, -1},
    {"Volume chip", 280, 0, 319, 35, 1, -1},
    {"Last list row", 0, 198, 289, 239, 2, 0},
    {"Remove", 6, 196, 134, 239, 3, 1},
    {"Play next", 140, 196, 226, 239, 3, 1},
    {"Clear all", 232, 196, 314, 239, 3, 1},
    {"Undo", 230, 199, 309, 235, 4, 2},
};
constexpr int kTargetCount = sizeof(kTargets) / sizeof(kTargets[0]);
const char* const kZoneNames[] = {"tabs", "volume chip", "last list row", "edit bar", "toast undo"};

// Haptic tiles: 3 x 3.
struct HapticDef {
  const char* line1;
  const char* line2;
  uint16_t ms;
  uint8_t level;
  uint8_t count;
  uint16_t gap;
};
constexpr HapticDef kHaptics[9] = {
    {"15 ms", "medium", 15, Haptics::kMedium, 1, 0},  {"25 ms", "medium", 25, Haptics::kMedium, 1, 0},
    {"40 ms", "medium", 40, Haptics::kMedium, 1, 0},  {"15 ms", "strong", 15, Haptics::kStrong, 1, 0},
    {"25 ms", "strong", 25, Haptics::kStrong, 1, 0},  {"40 ms", "strong", 40, Haptics::kStrong, 1, 0},
    {"2 x 20 ms", "80 apart", 20, Haptics::kStrong, 2, 80}, {"buzz", "80 ms", 80, Haptics::kStrong, 1, 0},
    {"2 x 40 ms", "inert", 40, Haptics::kStrong, 2, 60},
};

int tileAt(int x, int y) {
  if (y < 40) return -1;
  const int c = (x - 5) / 105, r = (y - 40) / 66;
  if (c < 0 || c > 2 || r < 0 || r > 2) return -1;
  return r * 3 + c;
}

uint32_t rnd(uint32_t n) { return n ? esp_random() % n : 0; }

m5::Button_Class& button(int b) { return b == 0 ? M5.BtnA : b == 1 ? M5.BtnB : M5.BtnC; }

void text(const char* s, int x, int y, const lgfx::IFont* font, uint16_t fg, uint16_t bg, int padding = 0,
          textdatum_t datum = textdatum_t::middle_left) {
  auto& d = M5.Display;
  d.setFont(font);
  d.setTextColor(fg, bg);
  d.setTextDatum(datum);
  d.setTextPadding(padding);
  d.drawString(s, x, y);
}

}  // namespace

InputLab::InputLab(Haptics& haptics) : haptics_(haptics) {
  buttons_ = static_cast<ButtonSample*>(psramAlloc(kMaxSamples * sizeof(ButtonSample)));
  targets_ = static_cast<TargetSample*>(psramAlloc(kMaxSamples * sizeof(TargetSample)));
  touches_ = static_cast<TouchSample*>(psramAlloc(kMaxSamples * sizeof(TouchSample)));
}

InputLab::~InputLab() {
  psramFree(buttons_);
  psramFree(targets_);
  psramFree(touches_);
}

void InputLab::open(int mode) {
  if (!ready()) {
    Serial.println("[input] no PSRAM for the lab");
    return;
  }
  if (mode < Free || mode > HapticTest) mode = Free;
  active_ = true;
  mode_ = mode;
  pending_ = Pending{};
  nextPromptAtMs_ = 0;
  target_ = -1;
  deckPos_ = deckLen_ = 0;
  bdeckPos_ = 6;
  promptLive_ = false;
  readoutLine_ = 0;
  lastDotX_ = lastDotY_ = -1;
  const char* names[] = {"free (live readout)", "tab target practice", "button practice", "haptic test"};
  Serial.printf("[input] lab open: u%d %s; buttons are logged, not acted on (u closes). Hold threshold %lu ms, "
                "click-count window %lu ms\n",
                mode, names[mode], (unsigned long)M5.BtnA.getHoldThresh(), (unsigned long)M5.BtnA.getHoldThresh());
  drawFrame();
  if (mode_ == Targets) nextTarget();
  if (mode_ == Buttons) nextButtonPrompt();
}

void InputLab::close() {
  if (!active_) return;
  active_ = false;
  haptics_.stop();
  Serial.printf("[input] lab closed (%lu button presses, %lu touches, %lu target attempts logged since boot; us "
                "prints the summary)\n",
                (unsigned long)nButtons_, (unsigned long)nTouches_, (unsigned long)nTargets_);
}

void InputLab::resetStats() {
  nButtons_ = nTargets_ = nTouches_ = 0;
  Serial.println("[input] stats cleared");
}

void InputLab::command(const char* arg) {
  if (!arg || !*arg) {
    if (active_) {
      close();
    } else {
      open(Free);
    }
    return;
  }
  const char c = arg[0];
  if (c >= '0' && c <= '3') {
    open(c - '0');
  } else if (c == 's') {
    summary();
  } else if (c == 'r') {
    resetStats();
  } else if (c == 'q') {
    close();
  } else if (c == 'h') {
    int ms = 25, level = Haptics::kMedium, count = 1, gap = 80;
    sscanf(arg + 1, "%d,%d,%d,%d", &ms, &level, &count, &gap);
    ms = constrain(ms, 1, 1000);
    if (level < Haptics::kMinLevel) {
      Serial.printf("[haptic] level %d is below %u: the AXP192's LDO3 would switch off (under 1.8 V); using %u\n",
                    level, Haptics::kMinLevel, Haptics::kMinLevel);
    }
    level = constrain(level, Haptics::kMinLevel, 255);
    count = constrain(count, 1, 4);
    gap = constrain(gap, 1, 1000);
    haptics_.pulses(static_cast<uint16_t>(ms), static_cast<uint8_t>(level), count, static_cast<uint16_t>(gap));
    Serial.printf("[haptic] console: %d x %d ms at level %d (LDO3 %d mV), %d ms apart\n", count, ms, level,
                  Haptics::motorMv(static_cast<uint8_t>(level)), gap);
    hapticReportAtMs_ = millis() + static_cast<uint32_t>(count * (ms + gap) + 50);
  } else if (c == 't') {
    const int ms = atoi(arg + 1);
    if (ms < 100 || ms > 3000) {
      Serial.println("[input] ut<ms>: hold threshold 100-3000 ms");
      return;
    }
    for (int b = 0; b < 3; ++b) button(b).setHoldThresh(static_cast<uint32_t>(ms));
    Serial.printf("[input] BtnA/B/C hold threshold %d ms (until restart; also the click-count window)\n", ms);
  } else {
    Serial.println("[input] u: toggle; u0 free, u1 targets, u2 buttons, u3 haptics; us summary; ur reset; "
                   "uh<ms>[,<level>[,<count>,<gap>]] haptic; ut<ms> hold threshold");
  }
}

// ---- drawing ----

void InputLab::drawFrame() {
  auto& d = M5.Display;
  fillLcd(0, kH, col::BG, &drawLock_);
  switch (mode_) {
    case Targets: break;  // drawn per prompt
    case Buttons: {
      LcdLock lock(&drawLock_);
      drawTabBar(d, -1);
      break;
    }
    case HapticTest: drawHaptics(); break;
    default: drawFree(); break;
  }
}

void InputLab::drawFree() {
  auto& d = M5.Display;
  d.fillRect(0, 0, kW, 34, col::SURF);
  text("Input lab: press the buttons, touch the glass", 8, 9, &fonts::Font2, col::TXT, col::SURF);
  text("u1 targets u2 buttons u3 haptics us summary u closes", 8, 25, &fonts::Font0, col::DIM, col::SURF);
  d.drawFastHLine(0, kGlassBottomY, kW, col::FAINT);  // the y >= 220 band
  text("y 220", 4, kGlassBottomY + 8, &fonts::Font0, col::FAINT, col::BG);
}

void InputLab::readout(const char* line) {
  if (mode_ != Free) return;
  const int y = 44 + readoutLine_ * 15;
  text(line, 6, y, &fonts::Font2, col::SOFT, col::BG, kW - 6);
  readoutLine_ = (readoutLine_ + 1) % 5;
  text(">", 0, 44 + readoutLine_ * 15, &fonts::Font2, col::CORAL, col::BG);
}

// Band by band, each under its own LcdLock (the bar, the header, each row,
// the bottom bar: a few ms of bus each), never the whole scene in one hold:
// the SD card, and so the decoder, can read between bands.
void InputLab::drawScene(Scene scene) {
  auto& d = M5.Display;
  {
    LcdLock lock(&drawLock_);
    drawTabBar(d, scene == SceneList ? 1 : 2);
  }
  {
    LcdLock lock(&drawLock_);
    d.fillRect(0, kHeaderY, kW, kListY - kHeaderY, col::HEAD);
  }
  const int rows = scene == SceneEdit ? 3 : 4;
  static const char* const kTitles[] = {"Harder, Better, Faster, Stronger", "Nightcall", "Can'T Tell Me Nothing",
                                        "Digital Love"};
  static const char* const kSubs[] = {"Daft Punk", "Kavinsky", "Kanye West", "Daft Punk"};
  for (int i = 0; i < rows; ++i) {
    LcdLock lock(&drawLock_);
    const int y = kListY + i * kRowH;
    d.fillRect(0, y, kW, kRowH, col::BG);
    int x = 12;
    if (scene == SceneEdit) {
      d.drawCircle(22, y + 21, 10, col::FAINT);
      x = 44;
    }
    text(kTitles[i], x, y + 14, &fonts::FreeSans9pt7b, col::TXT, col::BG);
    text(kSubs[i], x, y + 32, &fonts::Font2, col::DIM, col::BG);
    d.drawFastHLine(x, y + kRowH - 1, kW - x, col::ROW_DIV);
  }
  LcdLock lock(&drawLock_);
  if (scene == SceneEdit) {
    d.fillRect(0, 196, kW, 44, col::SURF);
    d.fillRoundRect(6, 202, 129, 32, 8, col::RED);
    text("Remove 2", 70, 218, &fonts::FreeSansBold9pt7b, col::CORAL_DK, col::RED, 0, textdatum_t::middle_center);
    d.fillRoundRect(140, 202, 87, 32, 8, col::BTN);
    text("Play next", 183, 218, &fonts::Font2, col::TXT, col::BTN, 0, textdatum_t::middle_center);
    d.drawRoundRect(232, 202, 83, 32, 8, col::RED);
    text("Clear all", 273, 218, &fonts::Font2, col::RED, col::SURF, 0, textdatum_t::middle_center);
  } else if (scene == SceneToast) {
    d.fillRoundRect(10, 199, 300, 37, 8, col::CARD);
    text("Added 14 tracks", 22, 217, &fonts::Font2, col::TXT, col::CARD);
    d.fillRoundRect(236, 204, 66, 27, 6, col::BTN);
    text("Undo", 269, 217, &fonts::FreeSansBold9pt7b, col::CORAL, col::BTN, 0, textdatum_t::middle_center);
  }
}

void InputLab::drawPrompt() {
  auto& d = M5.Display;
  LcdLock lock(&drawLock_);
  d.fillRect(0, kHeaderY, kW, kListY - kHeaderY, col::HEAD);
  if (target_ < 0) return;
  char line[48];
  snprintf(line, sizeof(line), "Tap:  %s", kTargets[target_].name);
  text(line, 10, 54, &fonts::FreeSansBold9pt7b, col::TXT, col::HEAD);
  snprintf(line, sizeof(line), "%d/%d", deckPos_, deckLen_);
  text(line, kW - 8, 54, &fonts::Font2, col::DIM, col::HEAD, 0, textdatum_t::middle_right);
  d.setTextDatum(textdatum_t::top_left);
}

void InputLab::drawButtonPrompt() {
  auto& d = M5.Display;
  fillLcd(kHeaderY, kH - kHeaderY, col::BG, &drawLock_);
  char line[48];
  if (promptIntent_ == IntentClick) {
    snprintf(line, sizeof(line), "Click  %c", kBtnName[promptButton_]);
  } else {
    snprintf(line, sizeof(line), "Hold  %c", kBtnName[promptButton_]);
  }
  text(line, kW / 2, 80, &fonts::FreeSansBold18pt7b, col::TXT, col::BG, 0, textdatum_t::middle_center);
  text(promptIntent_ == IntentClick ? "a normal press, like pause" : "until you'd expect it to act (about 1 s)",
       kW / 2, 112, &fonts::Font2, col::DIM, col::BG, 0, textdatum_t::middle_center);
  for (int b = 0; b < 3; ++b) {
    const int cx = 53 + b * 107;
    const bool on = b == promptButton_;
    d.fillCircle(cx, 212, 16, on ? col::CORAL : col::BTN);
    char s[2] = {kBtnName[b], 0};
    text(s, cx, 212, &fonts::FreeSansBold9pt7b, on ? col::CORAL_DK : col::SOFT, on ? col::CORAL : col::BTN, 0,
         textdatum_t::middle_center);
  }
  text("the buttons are the red dots under the screen", kW / 2, 234, &fonts::Font0, col::FAINT, col::BG, 0,
       textdatum_t::middle_center);
  d.setTextDatum(textdatum_t::top_left);
}

void InputLab::drawHaptics() {
  auto& d = M5.Display;
  {
    LcdLock lock(&drawLock_);
    d.fillRect(0, 0, kW, 34, col::SURF);
  }
  text("Haptics: tap a tile, say which feels right", 8, 17, &fonts::Font2, col::TXT, col::SURF);
  for (int i = 0; i < 9; ++i) {
    const int x = 5 + (i % 3) * 105, y = 40 + (i / 3) * 66;
    const bool hi = i == lastHaptic_;
    const uint16_t bg = hi ? col::BTN_HI : col::BTN;
    LcdLock lock(&drawLock_);
    d.fillRoundRect(x, y, 100, 62, 8, bg);
    text(kHaptics[i].line1, x + 50, y + 22, &fonts::FreeSansBold9pt7b, col::TXT, bg, 0, textdatum_t::middle_center);
    text(kHaptics[i].line2, x + 50, y + 44, &fonts::Font2, col::DIM, bg, 0, textdatum_t::middle_center);
  }
  d.setTextDatum(textdatum_t::top_left);
}

// ---- prompts ----

void InputLab::nextTarget() {
  if (deckPos_ >= deckLen_) {
    // A round: every tab and the chip once, the bottom-row targets twice, shuffled.
    deckLen_ = 0;
    for (int t = 0; t < kTargetCount; ++t) {
      const int reps = kTargets[t].zone <= ZoneChip ? 1 : 2;
      for (int r = 0; r < reps; ++r) deck_[deckLen_++] = static_cast<uint8_t>(t);
    }
    for (int i = deckLen_ - 1; i > 0; --i) {
      const int j = static_cast<int>(rnd(static_cast<uint32_t>(i + 1)));
      std::swap(deck_[i], deck_[j]);
    }
    deckPos_ = 0;
  }
  target_ = deck_[deckPos_++];
  const int8_t s = kTargets[target_].scene;
  scene_ = s >= 0 ? static_cast<Scene>(s) : static_cast<Scene>(rnd(kScenes));
  drawScene(scene_);
  drawPrompt();
  promptShownMs_ = millis();
}

void InputLab::nextButtonPrompt() {
  if (bdeckPos_ >= 6) {
    // Clicks first (A, B, C shuffled), then holds.
    uint8_t clicks[3] = {0, 1, 2}, holds[3] = {0, 1, 2};
    for (int i = 2; i > 0; --i) {
      std::swap(clicks[i], clicks[rnd(static_cast<uint32_t>(i + 1))]);
      std::swap(holds[i], holds[rnd(static_cast<uint32_t>(i + 1))]);
    }
    for (int i = 0; i < 3; ++i) {
      bdeck_[i] = clicks[i];
      bdeck_[3 + i] = static_cast<uint8_t>(3 + holds[i]);
    }
    bdeckPos_ = 0;
  }
  const uint8_t p = bdeck_[bdeckPos_++];
  promptButton_ = p % 3;
  promptIntent_ = p < 3 ? IntentClick : IntentHold;
  promptLive_ = true;
  drawButtonPrompt();
}

// ---- input ----

void InputLab::noteButtonEvent(uint32_t ms) { btnEvents_[btnEventHead_++ % 16] = ms; }

// The first button event within the window around [from, to]: its time
// relative to `from` (INT_MIN: none).
int InputLab::nearestButtonEventMs(uint32_t from, uint32_t to, int* who) const {
  int best = INT_MIN;
  for (int i = 0; i < 16; ++i) {
    const uint32_t t = btnEvents_[i];
    if (t == 0) continue;
    if (static_cast<int32_t>(t - (from - kButtonWindowMs)) < 0 || static_cast<int32_t>(t - (to + kButtonWindowMs)) > 0)
      continue;
    const int rel = static_cast<int>(static_cast<int32_t>(t - from));
    if (best == INT_MIN || abs(rel) < abs(best)) {
      best = rel;
      if (who) *who = btnEventWho_[i];
    }
  }
  return best;
}

void InputLab::pollButtons(uint32_t nowMs) {
  for (int b = 0; b < 3; ++b) {
    m5::Button_Class& B = button(b);
    BtnTrack& t = btn_[b];
    if (B.wasPressed()) {
      t = BtnTrack{};
      t.down = true;
      t.downMs = B.lastChange();
      // The raw touch point in this button's third of the strip, and its
      // finger's track (from the earlier passes: M5Unified derives the
      // buttons from the same touches, so a finger that landed on the glass
      // and rolled onto the strip was being tracked before this press).
      int id = -1;
      for (int i = 0; i < M5.Touch.getCount(); ++i) {
        m5gfx::touch_point_t tp = M5.Touch.getTouchPointRaw(i);
        M5.Display.convertRawXY(&tp, 1);
        if (tp.y >= kLcdH && tp.x * 3 / kW == b) {
          t.x = tp.x;
          t.y = tp.y;
          id = M5.Touch.getDetail(i).id < 5 ? M5.Touch.getDetail(i).id : 0;
        }
      }
      const TouchTrack* finger = id >= 0 && touch_[id].active ? &touch_[id] : nullptr;
      if (finger) {
        t.touchDownY = finger->downY;
        t.leadMs = t.downMs - finger->downMs;
        if (finger->bandY >= 0) {
          t.glassBottom = t.sameFinger = true;
          t.glassY = finger->bandY;
        }
      } else {
        t.touchDownY = t.y;  // it landed on the strip in this very pass
      }
      // Any finger in the band within 300 ms before the press.
      if (!t.glassBottom && lastBandY_ >= 0 && t.downMs - lastBandMs_ <= kButtonWindowMs) {
        t.glassBottom = true;
        t.glassY = lastBandY_;
      }
      btnEventWho_[btnEventHead_ % 16] = static_cast<uint8_t>(b);
      noteButtonEvent(t.downMs);
      Serial.printf("[btn] %c down t=%lu touch=(%d,%d) finger down at y=%d %lums before%s\n", kBtnName[b],
                    (unsigned long)t.downMs, t.x, t.y, t.touchDownY, (unsigned long)t.leadMs,
                    t.sameFinger ? " (it was on the glass at y 220-239 first)" : "");
      char line[64];
      snprintf(line, sizeof(line), "Btn%c down  touch (%d,%d)", kBtnName[b], t.x, t.y);
      readout(line);
    }
    if (B.wasHold() && t.down) {
      t.sawHold = true;
      t.holdAfterMs = nowMs - t.downMs;
      Serial.printf("[btn] %c m5 hold at %lu ms\n", kBtnName[b], (unsigned long)t.holdAfterMs);
    }
    if (B.wasReleased() && t.down) {
      t.down = false;
      const uint32_t upMs = B.lastChange();
      const uint32_t dur = upMs - t.downMs;
      const bool click = B.wasClicked();
      btnEventWho_[btnEventHead_ % 16] = static_cast<uint8_t>(b);
      noteButtonEvent(upMs);
      const bool prompted = mode_ == Buttons && promptLive_;
      const uint8_t intent = prompted ? static_cast<uint8_t>(promptIntent_) : static_cast<uint8_t>(IntentNone);
      t.sampleIdx = -1;
      if (nButtons_ < kMaxSamples) {
        t.sampleIdx = static_cast<int32_t>(nButtons_);
        buttons_[nButtons_++] = {static_cast<uint8_t>(b),
                                 intent,
                                 static_cast<uint8_t>(prompted ? promptButton_ : 255),
                                 click,
                                 t.sawHold,
                                 t.glassBottom,
                                 t.sameFinger,
                                 t.touchDownY,
                                 static_cast<uint16_t>(t.leadMs > 65535 ? 65535 : t.leadMs),
                                 static_cast<uint16_t>(dur > 65535 ? 65535 : dur),
                                 upMs};
      }
      t.afterUntilMs = upMs + kButtonWindowMs;
      char glass[40] = "no";
      if (t.glassBottom) {
        snprintf(glass, sizeof(glass), "yes (y=%d%s)", t.glassY, t.sameFinger ? ", same finger" : "");
      }
      char want[32] = "";
      if (prompted) {
        snprintf(want, sizeof(want), " asked=%s-%c%s", promptIntent_ == IntentClick ? "click" : "hold",
                 kBtnName[promptButton_], promptButton_ == b ? "" : " WRONG BUTTON");
      }
      Serial.printf("[btn] %c up t=%lu dur=%lums m5=%s (hold thresh %lums) touch=(%d,%d) glass>=220=%s%s\n",
                    kBtnName[b], (unsigned long)upMs, (unsigned long)dur,
                    click ? "click" : t.sawHold ? "hold" : "none", (unsigned long)B.getHoldThresh(), t.x, t.y,
                    glass, want);
      char line[64];
      snprintf(line, sizeof(line), "Btn%c %lu ms  %s  glass>=220 %s", kBtnName[b], (unsigned long)dur,
               click ? "click" : t.sawHold ? "hold" : "-", t.glassBottom ? "YES" : "no");
      readout(line);
      if (prompted) {
        promptLive_ = false;
        auto& d = M5.Display;
        d.fillRect(0, 130, kW, 60, col::BG);
        const bool ok = promptButton_ == b && (promptIntent_ == IntentClick ? click : t.sawHold);
        snprintf(line, sizeof(line), "%c %s: %lu ms", kBtnName[b], click ? "click" : t.sawHold ? "hold" : "?",
                 (unsigned long)dur);
        text(line, kW / 2, 148, &fonts::FreeSansBold12pt7b, ok ? col::GREEN : col::AMBER, col::BG, 0,
             textdatum_t::middle_center);
        snprintf(line, sizeof(line), "glass touch at y>=220: %s", t.glassBottom ? "YES" : "no");
        text(line, kW / 2, 174, &fonts::Font2, t.glassBottom ? col::AMBER : col::DIM, col::BG, 0,
             textdatum_t::middle_center);
        d.setTextDatum(textdatum_t::top_left);
        nextPromptAtMs_ = nowMs + 900;
      }
      // u1: a button with no glass touch around it may be a tap that landed on the strip.
      if (mode_ == Targets && target_ >= 0 && !pending_.active && !glassDown_ &&
          !(lastGlassEndMs_ && nowMs - lastGlassEndMs_ < kButtonWindowMs)) {
        pending_ = Pending{};
        pending_.active = true;
        pending_.buttonOnly = true;
        pending_.button = b;
        pending_.x = t.x;
        pending_.y = t.y;
        pending_.fromMs = t.downMs;
        pending_.toMs = upMs;
        pending_.decideAtMs = nowMs + kButtonWindowMs;
      }
    }
    if (B.wasDecideClickCount()) {
      Serial.printf("[btn] %c m5 decided %u click%s%s\n", kBtnName[b], B.getClickCount(),
                    B.getClickCount() == 1 ? "" : "s", B.wasDoubleClicked() ? " (double click)" : "");
    }
  }
}

void InputLab::pollTouches(uint32_t nowMs) {
  bool seen[5] = {};
  const int n = M5.Touch.getCount();
  for (int i = 0; i < n; ++i) {
    const auto& det = M5.Touch.getDetail(i);
    const int id = det.id < 5 ? det.id : 0;
    TouchTrack& t = touch_[id];
    if (det.isPressed()) {
      m5gfx::touch_point_t tp = M5.Touch.getTouchPointRaw(i);
      M5.Display.convertRawXY(&tp, 1);
      seen[id] = true;
      if (!t.active) {
        t.active = true;
        t.glass = tp.y < kLcdH;
        t.downMs = nowMs;
        t.downY = tp.y;
        t.bandY = -1;
        t.gesture.down(nowMs, tp.x, tp.y);
        if (t.glass) glassDown_ = true;
        if (mode_ == Free && t.glass) {
          if (lastDotX_ >= 0) M5.Display.fillCircle(lastDotX_, lastDotY_, 4, col::BG);
        }
      } else {
        t.gesture.move(nowMs, tp.x, tp.y);
      }
      t.lastSeenMs = nowMs;
      if (tp.y >= kGlassBottomY && tp.y < kLcdH) {
        t.bandY = tp.y;
        lastBandMs_ = nowMs;
        lastBandY_ = tp.y;
        for (int bi = 0; bi < 3; ++bi) {
          BtnTrack& b = btn_[bi];
          if (b.down && !b.glassBottom) {  // another finger, during the press
            b.glassBottom = true;
            b.glassY = tp.y;
          } else if (!b.down && b.afterUntilMs && static_cast<int32_t>(nowMs - b.afterUntilMs) <= 0) {
            // Just released: the finger rolled up off the strip onto the
            // glass (M5Unified released the button in this same update,
            // before this sample was seen), or another finger right after.
            // The same finger if this touch was already down when the button was pressed.
            const bool same = static_cast<int32_t>(t.downMs - b.downMs) <= 0;
            const uint32_t afterMs = nowMs - (b.afterUntilMs - kButtonWindowMs);
            b.afterUntilMs = 0;
            if (b.sampleIdx >= 0 && static_cast<uint32_t>(b.sampleIdx) < nButtons_ &&
                !buttons_[b.sampleIdx].glassBottom) {
              buttons_[b.sampleIdx].glassBottom = true;
              buttons_[b.sampleIdx].sameFinger = same;
              Serial.printf("[btn] %c: glass touch at y=%d %lu ms after the release (%s)\n", kBtnName[bi], tp.y,
                            (unsigned long)afterMs, same ? "the same finger, rolled up off the strip" : "another touch");
            }
          }
        }
      }
      if (mode_ == Free && t.glass && tp.y < kLcdH && tp.y > 36) {
        if (lastDotX_ >= 0 && (lastDotX_ != tp.x || lastDotY_ != tp.y)) {
          M5.Display.fillCircle(lastDotX_, lastDotY_, 4, col::FAINT);
        }
        M5.Display.fillCircle(tp.x, tp.y, 4, col::CORAL);
        lastDotX_ = tp.x;
        lastDotY_ = tp.y;
      }
    } else if (det.wasReleased() && t.active) {
      t.active = false;
      onTouchEnd(t.gesture.up(nowMs), t.glass, nowMs);
    }
  }
  for (int id = 0; id < 5; ++id) {  // a release the detail list didn't report
    TouchTrack& t = touch_[id];
    if (t.active && !seen[id] && nowMs - t.lastSeenMs > 80) {
      t.active = false;
      onTouchEnd(t.gesture.up(t.lastSeenMs), t.glass, nowMs);
    }
  }
  glassDown_ = false;
  for (const TouchTrack& t : touch_) glassDown_ |= t.active && t.glass;
}

void InputLab::onTouchEnd(const TouchGesture::Result& r, bool glass, uint32_t nowMs) {
  if (glass) lastGlassEndMs_ = nowMs;
  if (nTouches_ < kMaxSamples) {
    touches_[nTouches_++] = {static_cast<uint8_t>(r.kind), static_cast<uint16_t>(r.durationMs > 65535 ? 65535 : r.durationMs),
                             static_cast<uint16_t>(r.maxMovePx), static_cast<uint16_t>(r.speed > 65535 ? 65535 : r.speed),
                             static_cast<int16_t>(r.downY)};
  }
  Serial.printf("[touch] %s zone=%s down=(%d,%d) up=(%d,%d) dur=%lums move=%dpx v=(%.0f,%.0f)px/s samples=%lu\n",
                TouchGesture::name(r.kind), glass ? "glass" : "button-strip", r.downX, r.downY, r.upX, r.upY,
                (unsigned long)r.durationMs, r.maxMovePx, r.vx, r.vy, (unsigned long)r.samples);
  char line[64];
  snprintf(line, sizeof(line), "%s (%d,%d) %lu ms  move %d  %.0f px/s", TouchGesture::name(r.kind), r.downX, r.downY,
           (unsigned long)r.durationMs, r.maxMovePx, r.speed);
  readout(line);
  // Strip touches only matter to u1: a tap aimed at a bottom-row target that
  // landed below the LCD is a miss (with its offset), button or not.
  if (!glass && mode_ != Targets) return;

  if (mode_ == HapticTest && r.kind == TouchGesture::Kind::Tap) {
    const int tile = tileAt(r.downX, r.downY);
    if (tile >= 0) playHaptic(tile);
    return;
  }
  if (mode_ != Targets || target_ < 0 || nowMs < nextPromptAtMs_) return;
  if (pending_.active && !pending_.buttonOnly) return;  // one attempt at a time
  // A button-only attempt still waiting becomes this touch's (the button is then false).
  pending_.active = true;
  pending_.buttonOnly = false;
  pending_.touch = r;
  pending_.fromMs = r.downMs;
  pending_.toMs = r.downMs + r.durationMs;
  pending_.decideAtMs = nowMs + kButtonWindowMs;
}

int InputLab::targetAt(int x, int y) const {
  for (int t = 0; t < kTargetCount; ++t) {
    const TargetDef& d = kTargets[t];
    if (d.scene >= 0 && d.scene != scene_) continue;
    if (x >= d.x0 && x <= d.x1 && y >= d.y0 && y <= d.y1) return t;
  }
  return -1;
}

void InputLab::finishAttempt(uint32_t nowMs) {
  const TargetDef& want = kTargets[target_];
  TargetSample s{};
  s.target = static_cast<uint8_t>(target_);
  int who = -1;
  const int rel = nearestButtonEventMs(pending_.fromMs, pending_.toMs, &who);
  char btn[32] = "none";
  if (rel != INT_MIN) snprintf(btn, sizeof(btn), "%c (%+dms from the press)", kBtnName[who < 0 ? 0 : who], rel);
  ++attempts_;
  bool hit = false;
  if (pending_.buttonOnly) {
    s.buttonOnly = true;
    s.falseButton = true;
    s.strip = true;
    const int cx = (want.x0 + want.x1) / 2, cy = (want.y0 + want.y1) / 2;
    s.hasOffset = pending_.x >= 0 && pending_.y >= 0;
    s.dx = static_cast<int16_t>(s.hasOffset ? pending_.x - cx : 0);
    s.dy = static_cast<int16_t>(s.hasOffset ? pending_.y - cy : 0);
    Serial.printf("[target] #%lu %s (%s): MISS, no touch recorded, button %c fired (%lums press) at (%d,%d) "
                  "off=(%+d,%+d)\n",
                  (unsigned long)attempts_, want.name, kZoneNames[want.zone], kBtnName[pending_.button],
                  (unsigned long)(pending_.toMs - pending_.fromMs), pending_.x, pending_.y, s.dx, s.dy);
  } else {
    const TouchGesture::Result& r = pending_.touch;
    const int cx = (want.x0 + want.x1) / 2, cy = (want.y0 + want.y1) / 2;
    hit = r.downX >= want.x0 && r.downX <= want.x1 && r.downY >= want.y0 && r.downY <= want.y1;
    const bool upHit = r.upX >= want.x0 && r.upX <= want.x1 && r.upY >= want.y0 && r.upY <= want.y1;
    s.hit = hit;
    s.falseButton = rel != INT_MIN;
    s.strip = r.downY >= kLcdH;
    s.hasOffset = true;
    s.kind = static_cast<uint8_t>(r.kind);
    s.dx = static_cast<int16_t>(r.downX - cx);
    s.dy = static_cast<int16_t>(r.downY - cy);
    s.durMs = static_cast<uint16_t>(r.durationMs > 65535 ? 65535 : r.durationMs);
    const int got = targetAt(r.downX, r.downY);
    // Where the input layer's correction puts the press.
    char cal[80] = "";
    if (cal_) {
      const int x = cal_->mapX(r.downX), y = cal_->mapY(r.downY);
      const bool calHit = x >= want.x0 && x <= want.x1 && y >= want.y0 && y <= want.y1;
      snprintf(cal, sizeof(cal), " cal=(%d,%d) cal_off=(%+d,%+d) cal_hit=%s%s", x, y, x - cx, y - cy,
               calHit ? "yes" : "no", TouchCalibration::clampedHighX(r.downX) ? " (clamped)" : "");
    }
    Serial.printf("[target] #%lu %s (%s): %s down=(%d,%d) off=(%+d,%+d) up=(%d,%d)%s %s dur=%lums move=%dpx "
                  "hit=%s button=%s%s\n",
                  (unsigned long)attempts_, want.name, kZoneNames[want.zone], hit ? "HIT" : "MISS", r.downX, r.downY,
                  s.dx, s.dy, r.upX, r.upY, upHit ? "" : " (up outside)", TouchGesture::name(r.kind),
                  (unsigned long)r.durationMs, r.maxMovePx,
                  got >= 0 ? kTargets[got].name : s.strip ? "the button strip" : "nothing", btn, cal);
  }
  if (nTargets_ < kMaxSamples) targets_[nTargets_++] = s;
  // Feedback: the target's outline, green or red, then the next prompt.
  LcdLock lock(&drawLock_);
  M5.Display.drawRect(want.x0, want.y0, want.x1 - want.x0 + 1, want.y1 - want.y0 + 1, hit ? col::GREEN : col::RED);
  M5.Display.drawRect(want.x0 + 1, want.y0 + 1, want.x1 - want.x0 - 1, want.y1 - want.y0 - 1,
                      hit ? col::GREEN : col::RED);
  pending_ = Pending{};
  nextPromptAtMs_ = nowMs + kFeedbackMs;
}

void InputLab::playHaptic(int tile) {
  const HapticDef& h = kHaptics[tile];
  haptics_.pulses(h.ms, h.level, h.count, h.gap);
  lastHaptic_ = tile;
  drawHaptics();
  Serial.printf("[haptic] tile %d: %s %s (%u x %u ms at level %u = LDO3 %d mV%s)\n", tile, h.line1, h.line2, h.count,
                h.ms, h.level, Haptics::motorMv(h.level), h.count > 1 ? ", gaps" : "");
  hapticReportAtMs_ = millis() + h.count * (h.ms + h.gap) + 50;
}

void InputLab::loop(uint32_t nowMs) {
  if (!active_) return;
  pollButtons(nowMs);
  pollTouches(nowMs);
  if (mode_ == Targets) {
    if (pending_.active && static_cast<int32_t>(nowMs - pending_.decideAtMs) >= 0) finishAttempt(nowMs);
    if (!pending_.active && nextPromptAtMs_ && static_cast<int32_t>(nowMs - nextPromptAtMs_) >= 0) {
      nextPromptAtMs_ = 0;
      nextTarget();
    }
  }
  if (mode_ == Buttons && !promptLive_ && nextPromptAtMs_ && static_cast<int32_t>(nowMs - nextPromptAtMs_) >= 0) {
    bool anyDown = false;
    for (const BtnTrack& b : btn_) anyDown |= b.down;
    if (!anyDown) {
      nextPromptAtMs_ = 0;
      nextButtonPrompt();
    }
  }
  if (hapticReportAtMs_ && static_cast<int32_t>(nowMs - hapticReportAtMs_) >= 0 && !haptics_.busy()) {
    hapticReportAtMs_ = 0;
    Serial.printf("[haptic] motor on for %.1f ms in total (measured on the timer task)\n",
                  haptics_.lastOnUs() / 1000.0f);
  }
}

// ---- summary ----

void InputLab::summary() {
  float* v = static_cast<float*>(psramAlloc(kMaxSamples * sizeof(float)));
  float* w = static_cast<float*>(psramAlloc(kMaxSamples * sizeof(float)));
  if (!v || !w) {
    psramFree(v);
    psramFree(w);
    Serial.println("[summary] no memory");
    return;
  }
  auto line = [](const char* what, const Percentiles& p, const char* unit) {
    if (p.n == 0) {
      Serial.printf("[summary]   %-34s n=0\n", what);
      return;
    }
    Serial.printf("[summary]   %-34s n=%lu min=%.0f p10=%.0f p50=%.0f p90=%.0f p95=%.0f max=%.0f%s\n", what,
                  (unsigned long)p.n, p.min, p.p10, p.p50, p.p90, p.p95, p.max, unit);
  };
  Serial.printf("[summary] input lab, since boot: %lu button presses, %lu glass/strip touches, %lu target attempts; "
                "M5Unified hold threshold now %lu ms; the lab's screen draws held the bus %lu times, max %.2f ms\n",
                (unsigned long)nButtons_, (unsigned long)nTouches_, (unsigned long)nTargets_,
                (unsigned long)M5.BtnA.getHoldThresh(), (unsigned long)drawLock_.count, drawLock_.maxUs / 1000.0f);

  // Buttons: durations by intent (u2), and all presses by M5Unified's reading.
  for (int b = 0; b < 3; ++b) {
    char what[48];
    uint32_t n = 0, misread = 0, wrongButton = 0, glass = 0, total = 0;
    for (uint32_t i = 0; i < nButtons_; ++i) {
      const ButtonSample& s = buttons_[i];
      if (s.intent == IntentClick && s.prompted == b) {
        if (s.button != b) {
          ++wrongButton;
          continue;
        }
        v[n++] = s.durMs;
        if (!s.m5Click) ++misread;
      }
    }
    snprintf(what, sizeof(what), "Btn%c asked click: duration", kBtnName[b]);
    line(what, Percentiles::of(v, n), " ms");
    if (n || wrongButton) {
      Serial.printf("[summary]     read as hold by M5Unified: %lu of %lu; wrong button pressed: %lu\n",
                    (unsigned long)misread, (unsigned long)n, (unsigned long)wrongButton);
    }
    n = misread = wrongButton = 0;
    for (uint32_t i = 0; i < nButtons_; ++i) {
      const ButtonSample& s = buttons_[i];
      if (s.intent == IntentHold && s.prompted == b) {
        if (s.button != b) {
          ++wrongButton;
          continue;
        }
        v[n++] = s.durMs;
        if (!s.m5Hold) ++misread;
      }
    }
    snprintf(what, sizeof(what), "Btn%c asked hold: duration", kBtnName[b]);
    line(what, Percentiles::of(v, n), " ms");
    if (n || wrongButton) {
      Serial.printf("[summary]     released before M5Unified's hold (a click): %lu of %lu; wrong button: %lu\n",
                    (unsigned long)misread, (unsigned long)n, (unsigned long)wrongButton);
    }
    n = 0;
    uint32_t nh = 0, same = 0, landedOnGlass = 0, nDown = 0;
    for (uint32_t i = 0; i < nButtons_; ++i) {
      const ButtonSample& s = buttons_[i];
      if (s.button != b) continue;
      ++total;
      if (s.glassBottom) ++glass;
      if (s.sameFinger) ++same;
      if (s.touchDownY >= 0 && s.touchDownY < kLcdH) ++landedOnGlass;
      if (s.intent != IntentNone) continue;
      if (s.m5Click) v[n++] = s.durMs;
      if (s.m5Hold) w[nh++] = s.durMs;
    }
    snprintf(what, sizeof(what), "Btn%c unprompted clicks (M5)", kBtnName[b]);
    line(what, Percentiles::of(v, n), " ms");
    snprintf(what, sizeof(what), "Btn%c unprompted holds (M5)", kBtnName[b]);
    line(what, Percentiles::of(w, nh), " ms");
    Serial.printf("[summary]     presses with a glass touch at y 220-239 around them: %lu of %lu (the pressing "
                  "finger itself: %lu); pressing finger landed on the glass (y < 240): %lu\n",
                  (unsigned long)glass, (unsigned long)total, (unsigned long)same, (unsigned long)landedOnGlass);
    for (uint32_t i = 0; i < nButtons_; ++i) {
      if (buttons_[i].button == b && buttons_[i].touchDownY >= 0) v[nDown++] = buttons_[i].touchDownY;
    }
    snprintf(what, sizeof(what), "Btn%c pressing finger's down y", kBtnName[b]);
    line(what, Percentiles::of(v, nDown), " px");
  }

  // Targets, by zone.
  for (int z = 0; z < kZones; ++z) {
    uint32_t n = 0, hits = 0, falseBtn = 0, btnOnly = 0, strip = 0, nOff = 0;
    for (uint32_t i = 0; i < nTargets_; ++i) {
      const TargetSample& s = targets_[i];
      if (kTargets[s.target].zone != z) continue;
      ++n;
      if (s.hit) ++hits;
      if (s.falseButton) ++falseBtn;
      if (s.buttonOnly) ++btnOnly;
      if (s.strip) ++strip;
      if (s.hasOffset) {
        v[nOff] = s.dx;
        w[nOff] = s.dy;
        ++nOff;
      }
    }
    if (n == 0) {
      Serial.printf("[summary] target zone %-14s n=0\n", kZoneNames[z]);
      continue;
    }
    Serial.printf("[summary] target zone %-14s n=%lu hit=%.0f%% false-button=%.0f%% landed in the strip: %lu "
                  "(button with no touch recorded: %lu)\n",
                  kZoneNames[z], (unsigned long)n, 100.0f * hits / n, 100.0f * falseBtn / n, (unsigned long)strip,
                  (unsigned long)btnOnly);
    line("offset x from centre", Percentiles::of(v, nOff), " px");
    line("offset y from centre (+ = lower)", Percentiles::of(w, nOff), " px");
  }
  for (int t = 0; t < kTargetCount; ++t) {
    uint32_t n = 0, hits = 0, falseBtn = 0;
    for (uint32_t i = 0; i < nTargets_; ++i) {
      if (targets_[i].target != t) continue;
      ++n;
      hits += targets_[i].hit;
      falseBtn += targets_[i].falseButton;
    }
    if (n) {
      Serial.printf("[summary]   %-16s n=%lu hit=%lu false-button=%lu\n", kTargets[t].name, (unsigned long)n,
                    (unsigned long)hits, (unsigned long)falseBtn);
    }
  }

  // Glass touches: how still a tap is, how long, how fast a flick.
  uint32_t n = 0, nm = 0, counts[5] = {};
  for (uint32_t i = 0; i < nTouches_; ++i) {
    const TouchSample& s = touches_[i];
    if (s.kind < 5) counts[s.kind]++;
    if (s.kind == static_cast<uint8_t>(TouchGesture::Kind::Tap) ||
        s.kind == static_cast<uint8_t>(TouchGesture::Kind::Hold)) {
      w[nm++] = s.movePx;
    }
    if (s.kind == static_cast<uint8_t>(TouchGesture::Kind::Tap)) v[n++] = s.durMs;
  }
  Serial.printf("[summary] touches: tap %lu, hold %lu, drag %lu, flick %lu\n", (unsigned long)counts[1],
                (unsigned long)counts[2], (unsigned long)counts[3], (unsigned long)counts[4]);
  line("tap duration", Percentiles::of(v, n), " ms");
  line("tap/hold movement (slop 12)", Percentiles::of(w, nm), " px");
  n = 0;
  for (uint32_t i = 0; i < nTouches_; ++i) {
    if (touches_[i].kind == static_cast<uint8_t>(TouchGesture::Kind::Flick) ||
        touches_[i].kind == static_cast<uint8_t>(TouchGesture::Kind::Drag)) {
      v[n++] = touches_[i].speed;
    }
  }
  line("drag/flick release speed", Percentiles::of(v, n), " px/s");
  psramFree(v);
  psramFree(w);
}
