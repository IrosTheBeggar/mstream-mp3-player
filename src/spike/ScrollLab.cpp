#include "spike/ScrollLab.h"

#include <M5Unified.h>
#include <esp_random.h>
#include <esp_timer.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>

#include "Percentiles.h"
#include "TextFold.h"
#include "spike/SpikeUi.h"

using namespace spike;

namespace {

constexpr int kViewportH = kH - kListY;  // 168: 4 rows
constexpr int kThumbH = 30;
const char* const kViewNames[] = {"artists", "albums", "tracks"};
const char* const kPathNames[] = {"full redraw (wm0)", "hardware scroll (wm1)"};
const uint16_t kDiscColours[] = {0xFB49, 0x3EBE, 0xFE07, 0x4ECF, 0xA3DF, 0xFC9F};

int64_t nowUs() { return esp_timer_get_time(); }

uint32_t hashName(const char* s) {
  uint32_t h = 2166136261u;
  while (*s) h = (h ^ static_cast<unsigned char>(*s++)) * 16777619u;
  return h;
}

void chevron(M5Canvas& s, int x, int cy, uint16_t c) {
  s.drawLine(x, cy - 5, x + 5, cy, c);
  s.drawLine(x + 5, cy, x, cy + 5, c);
  s.drawLine(x + 1, cy - 5, x + 6, cy, c);
  s.drawLine(x + 6, cy, x + 1, cy + 5, c);
}

}  // namespace

ScrollLab::ScrollLab(Core2AudioBackend& audio, Input& input) : audio_(audio), input_(input) {
  seconds_ = static_cast<Second*>(psramAlloc(kMaxSeconds * sizeof(Second)));
}

ScrollLab::~ScrollLab() {
  close();
  dropSlots();
  if (rail_) {
    rail_->deleteSprite();
    psramDelete(rail_);
    rail_ = nullptr;
  }
  psramFree(seconds_);
}

bool ScrollLab::createSlots() {
  if (slots_) return true;
  void* mem = psramAlloc(kMaxSlots * sizeof(Slot));
  if (!mem) return false;
  slots_ = static_cast<Slot*>(mem);
  for (int i = 0; i < kMaxSlots; ++i) new (&slots_[i]) Slot();
  for (int i = 0; i < kMaxSlots; ++i) {
    M5Canvas& s = slots_[i].sprite;
    s.setPsram(true);  // before createSprite(): otherwise internal RAM
    s.setColorDepth(16);
    if (!s.createSprite(kW, kRowH)) {
      Serial.printf("[scroll] no PSRAM for row sprite %d\n", i);
      dropSlots();
      return false;
    }
  }
  return true;
}

void ScrollLab::dropSlots() {
  if (!slots_) return;
  for (int i = 0; i < kMaxSlots; ++i) {
    slots_[i].sprite.deleteSprite();
    slots_[i].~Slot();
  }
  psramFree(slots_);
  slots_ = nullptr;
}

uint32_t ScrollLab::count() const {
  if (!index_ || !index_->ready()) return 0;
  switch (view_) {
    case Artists: return index_->artistCount();
    case Albums: return index_->albumCount();
    default: return index_->trackCount();
  }
}

bool ScrollLab::open(LibraryIndex* index, int mode) {
  if (!index || !index->ready() || index->trackCount() == 0) {
    Serial.println("[scroll] no library index: g0 (the SD card) or g<n> (synthetic) first");
    return false;
  }
  if (!seconds_ || !createSlots()) {
    Serial.println("[scroll] no PSRAM for the scroll lab");
    return false;
  }
  index_ = index;
  mode_ = constrain(mode, 0, 3);
  active_ = true;
  for (int i = 0; i < kMaxSlots; ++i) slots_[i].row = -1;

  ScrollGovernor::Config g;
  g.enabled = mode_ == 1 || (mode_ == 0 && governed_);
  g.normalFrameMs = mode_ == 3 ? 0 : normalFrameMs_;
  gov_.setConfig(g);
  gov_.reset();
  // Flings are capped at the input layer's 2,000 px/s; the stress's own
  // flicks (wk) may go faster, to measure what that costs.
  KineticScroll::Config kc = scroll_.config();
  kc.maxPxPerS = std::max(KineticScroll::Config{}.maxPxPerS, mode_ >= 1 ? flickPxPerS_ : 0.0f);
  scroll_.setConfig(kc);
  scroll_.jumpTo(0);
  setView(view_);

  if (mode_ >= 1 && scroll_.maxOffset() < kMinStressPx) {
    Serial.printf("[scroll] w%d refused: the %s list scrolls only %.0f px (%lu rows); a stress needs %d px or more, or "
                  "most of each second is spent at an end. Build a synthetic library first (g2000: 120 artists, "
                  "g10000: 600), or wv2 for the tracks list (the card's 77 tracks do); g0 goes back to the card\n",
                  mode_, kViewNames[view_], scroll_.maxOffset(), (unsigned long)count(), kMinStressPx);
    active_ = false;
    return false;
  }
  if (mode_ >= 1 && scroll_.maxOffset() < kShortStressPx) {
    Serial.printf("[scroll] note: the %s list scrolls only %.0f px, under a second of a %.0f px/s flick: the stress "
                  "reverses at the ends often, and the jumps land near them\n",
                  kViewNames[view_], scroll_.maxOffset(), flickPxPerS_);
  }
  enterScreen();
  if (hw() && !startHw()) {
    Serial.println("[scroll] hardware scroll unavailable: full redraw (wm0) instead");
    path_ = Path::Redraw;
  }
  startSeen_ = audio_.startTiming().seq;
  watching_ = false;
  const uint32_t now = millis();
  secStartMs_ = now;
  lastLoopMs_ = now;
  nextFrameMs_ = now;
  touchOn_ = TouchOn::None;
  linesPushed_ = gapMaxMs_ = 0;
  frames_ = rowsDrawn_ = slicesPushed_ = 0;
  frameUs_ = drawUs_ = pushUs_ = 0;
  frameMaxUs_ = 0;
  movingMs_ = heldMs_ = 0;
  lock_.reset();
  ringMin_ = heapMin_ = UINT32_MAX;
  underruns0_ = audio_.underrunsNow();
  busy0_ = audio_.decodeBusyUsTotal();

  const RefillPacer::Config pace = audio_.refillPacing();
  Serial.printf("[scroll] open w%d: %s view, %lu rows, governor %s, frame cap %lu ms, slices of %d px, %d cached row "
                "sprite%s (%u KB PSRAM); %s, flings capped at %.0f px/s, refill pacing %s (%.1fx)\n",
                mode_, kViewNames[view_], (unsigned long)count(), g.enabled ? "on" : "off",
                (unsigned long)g.normalFrameMs, sliceH_, cacheRows_, cacheRows_ == 1 ? "" : "s",
                (unsigned)(kMaxSlots * kW * kRowH * 2 / 1024), kPathNames[static_cast<int>(path_)],
                scroll_.config().maxPxPerS, pace.enabled ? "on" : "off", pace.capX10 / 10.0f);
  stress_ = mode_ >= 1;
  if (stress_) {
    stressStartMs_ = now;
    nextActionMs_ = now + 500;
    actions_ = 0;
    flingDir_ = 1;
    reflickAfterJump_ = false;
    nSeconds_ = 0;
    runUnderruns0_ = audio_.underrunsNow();
    Serial.printf("[scroll] stress: %lu s of flicks at %.0f px/s up and down (reversed at the ends), an A-Z jump every "
                  "5th action and a flick straight after it%s\n",
                  (unsigned long)(stressMs_ / 1000), flickPxPerS_,
                  audio_.isPlaying() ? "" : "; NOTHING IS PLAYING (start a track with i<n> for the real test)");
  }
  return true;
}

void ScrollLab::close() {
  if (!active_) return;
  if (stress_) finishStress();
  // Leave the panel as every other screen expects it: no scroll offset, and
  // the band cleared first so the switch back shows nothing rotated.
  stopHw(true);
  watching_ = false;
  active_ = false;
  touchOn_ = TouchOn::None;
  Serial.println("[scroll] closed");
}

void ScrollLab::setView(View v) {
  view_ = v;
  if (slots_) {
    for (int i = 0; i < kMaxSlots; ++i) slots_[i].row = -1;
  }
  scroll_.setExtent(static_cast<float>(count()) * kRowH, kViewportH);
  scroll_.jumpTo(0);
  forceFrame_ = true;
  railThumbY_ = -1;
  railKey_ = 0;
  railBucket_ = -1;
  railUp_ = false;  // the frame redraws the band; the rail with it if it's wanted
}

void ScrollLab::enterScreen() {
  auto& d = M5.Display;
  fillLcd(0, kH, col::BG);
  {
    LcdLock lock;
    drawTabBar(d, static_cast<int>(Tab::Library));
  }
  drawHeader();
  forceFrame_ = true;
  drawnOffset_ = -1;
  railUp_ = false;
}

void ScrollLab::drawHeader() {
  auto& d = M5.Display;
  LcdLock lock;
  d.fillRect(0, kHeaderY, kW, kListY - kHeaderY, col::HEAD);
  d.fillRoundRect(6, 40, 308, 28, 8, col::CARD);
  const char* labels[] = {"Artists", "Albums", "Tracks"};
  for (int i = 0; i < 3; ++i) {
    const int x = 8 + i * 102;
    const bool on = i == view_;
    if (on) d.fillRoundRect(x, 42, 100, 24, 6, col::BTN_HI);
    d.setFont(on ? &fonts::FreeSansBold9pt7b : &fonts::FreeSans9pt7b);
    d.setTextColor(on ? col::TXT : col::DIM, on ? col::BTN_HI : col::CARD);
    d.setTextDatum(textdatum_t::middle_center);
    d.setTextPadding(0);
    d.drawString(labels[i], x + 50, 54);
  }
  d.setTextDatum(textdatum_t::top_left);
}

void ScrollLab::drawRow(M5Canvas& s, uint32_t i) {
  const int w = rowWidth();
  s.fillSprite(col::BG);
  s.setTextDatum(textdatum_t::middle_left);
  s.setTextPadding(0);
  char buf[112];
  char sub[112];
  int textX = 50;
  const char* primary = "";
  size_t primaryLen = 0;
  if (view_ == Artists) {
    const uint32_t id = index_->artistsAZ()[i];
    const char* name = index_->artistName(id);
    const LibraryIndex::Artist& a = index_->artist(id);
    const uint16_t c = kDiscColours[hashName(name) % 6];
    s.fillCircle(25, 21, 15, c);
    char key[2] = {textfold::railKey(name), 0};
    s.setFont(&fonts::FreeSansBold9pt7b);
    s.setTextColor(col::CORAL_DK, c);
    s.setTextDatum(textdatum_t::middle_center);
    s.drawString(key, 25, 21);
    s.setTextDatum(textdatum_t::middle_left);
    primary = name[0] ? name : "(no artist folder)";
    primaryLen = strlen(primary);
    snprintf(sub, sizeof(sub), "%lu album%s, %lu tracks", (unsigned long)a.albumCount, a.albumCount == 1 ? "" : "s",
             (unsigned long)a.trackCount);
  } else if (view_ == Albums) {
    const uint32_t id = index_->albumsAZ()[i];
    const char* name = index_->albumName(id);
    s.fillRoundRect(10, 5, 32, 32, 4, col::BTN);
    s.fillCircle(24, 27, 4, col::FAINT);  // a note
    s.fillRect(27, 11, 2, 16, col::FAINT);
    textX = 52;
    primary = name[0] ? name : "(loose tracks)";
    primaryLen = strlen(primary);
    s.setFont(&fonts::Font2);
    const char* artist = index_->artistName(index_->album(id).artist);
    fitText(s, artist[0] ? artist : "(no artist folder)", strlen(artist[0] ? artist : "(no artist folder)"), sub,
            sizeof(sub), w - textX - 24);
  } else {
    const uint32_t id = index_->allTracks()[i];
    const LibraryIndex::Track& t = index_->track(id);
    textX = 40;
    if (t.number) {
      char num[6];
      snprintf(num, sizeof(num), "%u", t.number);
      s.setFont(&fonts::Font2);
      s.setTextColor(col::DIM, col::BG);
      s.setTextDatum(textdatum_t::middle_right);
      s.drawString(num, 30, 14);
      s.setTextDatum(textdatum_t::middle_left);
    }
    uint8_t len = 0;
    primary = index_->trackTitle(id, &len);
    primaryLen = len;
    snprintf(sub, sizeof(sub), "%s - %s", index_->artistName(t.artist), index_->albumName(t.album));
  }
  // Primary line: FreeSans 9 pt, folded to ASCII, ellipsised to the column.
  s.setFont(&fonts::FreeSans9pt7b);
  s.setTextColor(col::TXT, col::BG);
  fitText(s, primary, primaryLen, buf, sizeof(buf), w - textX - (view_ == Tracks ? 8 : 24));
  s.drawString(buf, textX, 14);
  // Secondary: Font2, dim.
  s.setFont(&fonts::Font2);
  s.setTextColor(col::DIM, col::BG);
  if (view_ != Albums) {
    char tmp[112];
    fitText(s, sub, strlen(sub), tmp, sizeof(tmp), w - textX - 24);
    s.drawString(tmp, textX, 32);
  } else {
    s.drawString(sub, textX, 32);
  }
  if (view_ != Tracks) chevron(s, w - 18, 21, col::FAINT);
  s.drawFastHLine(textX, kRowH - 1, w - textX, col::ROW_DIV);
}

ScrollLab::Slot* ScrollLab::slotFor(uint32_t row, uint32_t firstVisible, uint32_t lastVisible) {
  // The hardware path uses every slot: a move renders all the rows its new
  // lines come from (up to 3) before it pushes any, and the rows at the
  // edges are pushed a part per frame, so they had better stay cached.
  const int n = hw() ? kMaxSlots : std::min(std::max(cacheRows_, 1), kMaxSlots);
  for (int i = 0; i < n; ++i) {
    if (slots_[i].row == static_cast<int32_t>(row)) return &slots_[i];
  }
  for (int i = 0; i < n; ++i) {
    const int32_t r = slots_[i].row;
    if (r < 0 || r < static_cast<int32_t>(firstVisible) || r > static_cast<int32_t>(lastVisible)) {
      slots_[i].row = -1;
      return &slots_[i];
    }
  }
  slots_[0].row = -1;  // one sprite: every row is redrawn
  return &slots_[0];
}

void ScrollLab::renderFrame(int offset) {
  auto& d = M5.Display;
  const uint32_t n = count();
  const int w = pushWidth();
  const uint32_t first = static_cast<uint32_t>(offset / kRowH);
  const uint32_t last = std::min<uint32_t>(n ? n - 1 : 0, static_cast<uint32_t>((offset + kViewportH - 1) / kRowH));
  int y = kListY - offset % kRowH;
  for (uint32_t i = first; y < kH; ++i, y += kRowH) {
    const int top = std::max(y, kListY);
    if (i >= n) {  // below the list's end
      const int64_t t0 = nowUs();
      {
        LcdLock lock(&lock_);
        d.fillRect(0, top, w, kH - top, col::BG);
      }
      pushUs_ += static_cast<uint64_t>(nowUs() - t0);
      break;
    }
    Slot* slot = slotFor(i, first, last);
    if (slot->row != static_cast<int32_t>(i)) {
      const int64_t t0 = nowUs();
      drawRow(slot->sprite, i);
      slot->row = static_cast<int32_t>(i);
      drawUs_ += static_cast<uint64_t>(nowUs() - t0);
      ++rowsDrawn_;
    }
    // Push the visible part in slices, each under its own lock, yielding between.
    const int bottom = std::min(y + kRowH, kH);
    for (int sy = top; sy < bottom; sy += sliceH_) {
      const int h = std::min(sliceH_, bottom - sy);
      const int64_t t0 = nowUs();
      {
        LcdLock lock(&lock_);
        d.setClipRect(0, sy, w, h);
        slot->sprite.pushSprite(&d, 0, y);
        d.clearClipRect();
      }
      pushUs_ += static_cast<uint64_t>(nowUs() - t0);
      ++slicesPushed_;
      taskYIELD();
    }
  }
}

// ---- the hardware-scroll path (wm1, wm2) ----

bool ScrollLab::createRailSprite() {
  if (rail_) return true;
  rail_ = psramNew<M5Canvas>();
  if (!rail_) return false;
  rail_->setPsram(true);  // before createSprite(): otherwise internal RAM
  rail_->setColorDepth(16);
  if (!rail_->createSprite(kW - kRailX, kViewportH)) {
    psramDelete(rail_);
    rail_ = nullptr;
    return false;
  }
  return true;
}

bool ScrollLab::startHw() {
  if (vscroll_.active()) return true;
  if (!createRailSprite()) {
    Serial.println("[scroll] no PSRAM for the rail sprite");
    return false;
  }
  // The tab bar (0-35) and the segmented header (36-71) are the fixed top
  // area; the list's 168 lines scroll; no bottom fixed area. Moves of more
  // than kHwMaxStep lines are full redraws in place (bounds the move's hold).
  if (!vscroll_.begin(kListY, kViewportH, &lock_, kHwMaxStep)) return false;
  forceFrame_ = true;
  return true;
}

void ScrollLab::stopHw(bool clearBand) {
  if (!vscroll_.active()) return;
  if (clearBand) fillLcd(kListY, kViewportH, col::BG, &lock_);  // uniform GRAM: nothing shows rotated
  vscroll_.end();
  forceFrame_ = true;
  railThumbY_ = -1;
  railUp_ = false;
}

void ScrollLab::setPath(int m) {
  if (m >= 2) Serial.println("[scroll] wm2 (hardware scroll + the interaction boost): the boost was removed, wm1");
  path_ = m >= 1 ? Path::Hardware : Path::Redraw;
  if (!active_) return;
  if (hw() && !startHw()) {
    Serial.println("[scroll] hardware scroll unavailable: staying on the full redraw");
    path_ = Path::Redraw;
  }
  // Back to the full redraw: clear the band before the address goes back to
  // the identity (its GRAM is in the rotated order), then the next frame
  // redraws every row and the rail at the identity.
  if (!hw()) stopHw(true);
  forceFrame_ = true;
  railThumbY_ = -1;
  railUp_ = false;
}

// The slot for `row` with the row drawn in it (drawn now if it isn't).
ScrollLab::Slot* ScrollLab::renderedSlot(int32_t row) {
  Slot* slot = slotFor(static_cast<uint32_t>(row), keepFirst_, keepLast_);
  if (slot->row != row) {
    const int64_t t0 = nowUs();
    drawRow(slot->sprite, static_cast<uint32_t>(row));
    slot->row = row;
    drawUs_ += static_cast<uint64_t>(nowUs() - t0);
    ++rowsDrawn_;
  }
  return slot;
}

// A move's new lines, before its commit: every row they come from drawn into
// a slot now, off the bus (at most 3 rows for kHwMaxStep; the hardware path
// has all 6 slots and keeps these).
void ScrollLab::prepare(const VScrollMap::Span* spans, int n) {
  if (n <= 0) return;
  const auto rows = static_cast<int32_t>(count());
  const int32_t first = spans[0].contentY;
  const int32_t end = spans[n - 1].contentY + spans[n - 1].h;  // spans are consecutive content lines
  const int32_t r0 = std::max<int32_t>(0, first / kRowH);
  const int32_t r1 = std::min<int32_t>(rows - 1, (end - 1) / kRowH);
  if (r1 < r0) return;  // all beyond the list
  keepFirst_ = static_cast<uint32_t>(r0);
  keepLast_ = static_cast<uint32_t>(r1);
  for (int32_t r = r0; r <= r1; ++r) renderedSlot(r);
}

// Content lines [span.contentY, + h) into GRAM lines [span.gramY, + h): row
// by row, only the lines needed. `committing`: under the move's bus hold,
// every row already prepared: whole runs, no yields. Otherwise (a full
// redraw in place) in `wh` slices under their own locks, with yields.
void ScrollLab::push(const VScrollMap::Span& span, bool committing) {
  auto& d = M5.Display;
  const auto n = static_cast<int32_t>(count());
  const int w = pushWidth();
  int32_t c = span.contentY;
  const int32_t end = span.contentY + span.h;
  int g = span.gramY;
  while (c < end) {
    const int32_t row = c >= 0 ? c / kRowH : -1;
    const int32_t rowTop = row * kRowH;
    const int32_t stop = std::min<int32_t>(end, c >= 0 ? rowTop + kRowH : 0);
    const int lines = static_cast<int>(stop - c);
    const int step = committing ? lines : sliceH_;
    if (row < 0 || row >= n) {  // beyond the list: background
      for (int k = 0; k < lines; k += kRowH) {
        const int h = std::min(kRowH, lines - k);
        const int64_t t0 = nowUs();
        {
          LcdLock lock(&lock_);
          d.fillRect(0, g + k, w, h, col::BG);
        }
        pushUs_ += static_cast<uint64_t>(nowUs() - t0);
      }
    } else {
      Slot* slot = renderedSlot(row);
      const int spriteY = g - static_cast<int>(c - rowTop);  // where the sprite's line 0 would be
      for (int k = 0; k < lines; k += step) {
        const int h = std::min(step, lines - k);
        const int64_t t0 = nowUs();
        {
          LcdLock lock(&lock_);
          d.setClipRect(0, g + k, w, h);
          slot->sprite.pushSprite(&d, 0, spriteY);
          d.clearClipRect();
        }
        pushUs_ += static_cast<uint64_t>(nowUs() - t0);
        ++slicesPushed_;
        if (!committing) taskYIELD();
      }
    }
    g += lines;
    c = stop;
  }
}

void ScrollLab::prepareFixed(int32_t offset) {
  railDue_ = railShown() && railWanted_ && rail_;
  if (!railDue_) return;
  const int64_t t0 = nowUs();
  drawRailSprite(static_cast<int>(offset));
  drawUs_ += static_cast<uint64_t>(nowUs() - t0);
}

// The rail sits in the scrolled band: the panel moves it with the list, so
// while it shows (a scrub) it goes back to its place at every move (30 x
// 168: ~2 ms of bus). While the list moves on its own it is hidden.
void ScrollLab::pushFixed() {
  if (!railDue_) return;
  railDue_ = false;
  const int64_t t0 = nowUs();
  vscroll_.pushAtScreen(*rail_, kRailX, kListY, &lock_);
  pushUs_ += static_cast<uint64_t>(nowUs() - t0);
  railUp_ = true;
}

bool ScrollLab::railWanted() const {
  return railShown() && (touchOn_ == TouchOn::Rail || !scroll_.moving());
}

// The rail's column, cleared (the whole band's height: every GRAM line of
// it, so the hardware scroll's rotation doesn't matter). The rows are then
// pushed full width over it until it shows again.
void ScrollLab::hideRail() {
  {
    LcdLock lock(&lock_);
    M5.Display.fillRect(kRailX, kListY, kW - kRailX, kViewportH, col::BG);
  }
  railUp_ = false;
  railThumbY_ = -1;
}

// The rail, drawn again where the list came to rest.
void ScrollLab::showRail(int offset) {
  if (hw()) {
    if (!rail_ || !vscroll_.active()) return;
    const int64_t t0 = nowUs();
    drawRailSprite(offset);
    vscroll_.pushAtScreen(*rail_, kRailX, kListY, &lock_);
    pushUs_ += static_cast<uint64_t>(nowUs() - t0);
  } else {
    drawRail(offset, true);
  }
  railUp_ = true;
}

// The whole rail, as drawRail() draws it on the LCD, into its sprite.
void ScrollLab::drawRailSprite(int offset) {
  M5Canvas& s = *rail_;
  const float maxOff = scroll_.maxOffset();
  const int thumbY = 2 + (maxOff > 0 ? static_cast<int>(offset * (kViewportH - kThumbH - 4) / maxOff) : 0);
  const uint32_t top = static_cast<uint32_t>(offset / kRowH);
  const int bucket = index_->bucketAt(view_ == Artists ? LibraryIndex::View::Artists : LibraryIndex::View::Albums, top);
  const char key[2] = {bucket == 0 ? '#' : static_cast<char>('A' + bucket - 1), 0};
  s.fillSprite(col::BG);
  s.drawFastVLine(307 - kRailX, 4, kViewportH - 8, col::DIV);
  s.fillRoundRect(297 - kRailX, thumbY, 20, kThumbH, 5, col::BTN_HI);
  s.setFont(&fonts::FreeSansBold9pt7b);
  s.setTextColor(col::TXT, col::BTN_HI);
  s.setTextDatum(textdatum_t::middle_center);
  s.setTextPadding(0);
  s.drawString(key, 307 - kRailX, thumbY + kThumbH / 2);
  s.setTextDatum(textdatum_t::top_left);
}

void ScrollLab::renderFrameHw(int offset) {
  if (forceFrame_) vscroll_.invalidate();
  const uint32_t n = count();
  // Until prepare() narrows it for a move: keep the rows on screen.
  const int top = std::max(offset, 0);
  keepFirst_ = static_cast<uint32_t>(top / kRowH);
  keepLast_ = std::min<uint32_t>(n ? n - 1 : 0, static_cast<uint32_t>((top + kViewportH - 1) / kRowH));
  uint32_t sendUs = 0;
  linesPushed_ += static_cast<uint32_t>(vscroll_.scrollTo(offset, *this, &sendUs));
  pushUs_ += sendUs;
}

// The longest frame and loop gap in the kStartWatchMs after each track start.
void ScrollLab::pollTrackStart(uint32_t nowMs, uint32_t dtMs) {
  const uint32_t seq = audio_.startTiming().seq;
  if (seq != startSeen_) {
    // The decode task counts the start when it takes the request, usually
    // while the loop is in a frame (it sleeps 10 ms at a time while the ring
    // is full), and then refills at priority 2: the stall lands in the frame
    // or loop gap that just ended, before this sees the new seq. So both
    // count (without this the watch read ~90 ms on the device for 800 ms
    // stalls that the per-second frame maximum caught).
    startSeen_ = seq;
    watching_ = true;
    watchStartMs_ = nowMs;
    watchFrames_ = 0;
    // The last frame only if the last pass drew it (not an old one while idle).
    watchFrameMaxUs_ = static_cast<int32_t>(lastFrameEndMs_ - (nowMs - dtMs)) >= 0 ? lastFrameUs_ : 0;
    watchGapMaxMs_ = dtMs;
    return;
  }
  if (!watching_) return;
  if (dtMs > watchGapMaxMs_) watchGapMaxMs_ = dtMs;
  if (nowMs - watchStartMs_ < kStartWatchMs) return;
  watching_ = false;
  const RefillPacer::Config pace = audio_.refillPacing();
  char paced[24] = "off";
  if (pace.enabled) snprintf(paced, sizeof(paced), "on, %.1fx", pace.capX10 / 10.0f);
  Serial.printf("[scroll] track start: in the %lu s after it, longest frame %.1f ms, longest loop gap %lu ms, %lu "
                "frames (%s, refill pacing %s)\n",
                (unsigned long)(kStartWatchMs / 1000), watchFrameMaxUs_ / 1000.0f, (unsigned long)watchGapMaxMs_,
                (unsigned long)watchFrames_, kPathNames[static_cast<int>(path_)], paced);
}

void ScrollLab::drawRail(int offset, bool force) {
  if (!railShown()) return;
  auto& d = M5.Display;
  const float maxOff = scroll_.maxOffset();
  const int thumbY = kListY + 2 + (maxOff > 0 ? static_cast<int>(offset * (kViewportH - kThumbH - 4) / maxOff) : 0);
  const uint32_t top = static_cast<uint32_t>(offset / kRowH);
  const int bucket = index_->bucketAt(view_ == Artists ? LibraryIndex::View::Artists : LibraryIndex::View::Albums, top);
  const char key = bucket == 0 ? '#' : static_cast<char>('A' + bucket - 1);
  if (!force && thumbY == railThumbY_ && key == railKey_) return;
  LcdLock lock(&lock_);
  if (force || railThumbY_ < 0) {
    d.fillRect(kRailX, kListY, kW - kRailX, kViewportH, col::BG);
  } else {
    d.fillRect(297, railThumbY_, 20, kThumbH, col::BG);
  }
  d.drawFastVLine(307, kListY + 4, kViewportH - 8, col::DIV);
  d.fillRoundRect(297, thumbY, 20, kThumbH, 5, col::BTN_HI);
  char s[2] = {key, 0};
  d.setFont(&fonts::FreeSansBold9pt7b);
  d.setTextColor(col::TXT, col::BTN_HI);
  d.setTextDatum(textdatum_t::middle_center);
  d.setTextPadding(0);
  d.drawString(s, 307, thumbY + kThumbH / 2);
  d.setTextDatum(textdatum_t::top_left);
  railThumbY_ = thumbY;
  railKey_ = key;
}

// Scrub: the list jumps to the proportional row; a tick at each new letter.
void ScrollLab::scrubRail(int y) {
  const float f = std::min(1.0f, std::max(0.0f, (y - kListY - kThumbH / 2) / static_cast<float>(kViewportH - kThumbH)));
  const float row = std::round(f * scroll_.maxOffset() / kRowH);
  scroll_.jumpTo(row * kRowH);
  const int b = index_->bucketAt(view_ == Artists ? LibraryIndex::View::Artists : LibraryIndex::View::Albums,
                                 static_cast<uint32_t>(row));
  if (b != railBucket_) {
    if (railBucket_ >= 0) input_.railTick();
    railBucket_ = b;
  }
}

void ScrollLab::onTouch(const InputEvent& e) {
  using T = InputEvent::Type;
  if (!active_) return;
  if (e.type == T::Down) {
    if (e.y < kBarH) {
      touchOn_ = TouchOn::Bar;
    } else if (e.y < kListY) {
      touchOn_ = TouchOn::Header;
    } else if (railShown() && e.inRightEdgeZone(kRailHitX)) {
      touchOn_ = TouchOn::Rail;  // shows the rail at once (railWanted())
    } else {
      touchOn_ = TouchOn::List;
    }
    if (stress_) {
      Serial.println("[scroll] touch: stress stopped");
      finishStress();
    }
    if (touchOn_ == TouchOn::List) scroll_.press(e.ms, e.y);
    if (touchOn_ == TouchOn::Rail) scrubRail(e.y);
    return;
  }
  const bool ends = e.type == T::Tap || e.type == T::Release || e.type == T::DragEnd || e.type == T::Cancel;
  switch (touchOn_) {
    case TouchOn::List:
      if (e.type == T::DragStart || e.type == T::DragMove) {
        scroll_.drag(e.ms, e.y);
      } else if (e.type == T::DragEnd) {
        scroll_.release(e.ms, -e.vy);  // the input layer's velocity, capped; the list moves against the finger
      } else if (ends) {
        scroll_.release(e.ms, 0.0f);   // a tap or a hold: snap to a row
      }
      break;
    case TouchOn::Rail:
      if (e.type == T::DragStart || e.type == T::DragMove) scrubRail(e.y);
      break;
    case TouchOn::Header:
      if (e.type == T::Tap) {
        const int seg = std::min(2, std::max(0, (e.x - 8) / 102));
        if (seg != view_) {
          setView(static_cast<View>(seg));
          drawHeader();
          Serial.printf("[scroll] view: %s, %lu rows\n", kViewNames[view_], (unsigned long)count());
        }
      }
      break;
    case TouchOn::Bar:
      if (e.type == T::Tap && e.x >= kTabW && e.x < 2 * kTabW) scroll_.jumpTo(0);  // the Library tab again: the top
      break;
    default:
      break;
  }
  if (ends) touchOn_ = TouchOn::None;
}

// Keeps the list moving for the whole run, so the numbers describe a list
// in motion: a flick reverses the moment it reaches an end (not at the next
// action), and an A-Z jump is followed by a flick as soon as it is drawn.
void ScrollLab::stressStep(uint32_t nowMs) {
  if (nowMs - stressStartMs_ >= stressMs_) {
    finishStress();
    return;
  }
  const float off = scroll_.offset();
  const bool atEnd = off >= scroll_.maxOffset() - 1 || off <= 1;
  if (reflickAfterJump_ && !forceFrame_) {
    // The jump's frame is out: flick on, towards the side with more room.
    reflickAfterJump_ = false;
    flingDir_ = off > scroll_.maxOffset() / 2 ? -1 : 1;
    scroll_.fling(nowMs, flingDir_ * flickPxPerS_);
    nextActionMs_ = nowMs + 700;
    return;
  }
  if (!reflickAfterJump_ && (!scroll_.moving() || scroll_.phase() == KineticScroll::Phase::Snapping) && atEnd) {
    flingDir_ = off <= 1 ? 1 : -1;
    scroll_.fling(nowMs, flingDir_ * flickPxPerS_);
    nextActionMs_ = nowMs + 700;
    return;
  }
  if (static_cast<int32_t>(nowMs - nextActionMs_) < 0) return;
  ++actions_;
  if (actions_ % 5 == 0 && view_ != Tracks) {
    // An A-Z jump to a random letter that has entries.
    const LibraryIndex::View v = view_ == Artists ? LibraryIndex::View::Artists : LibraryIndex::View::Albums;
    uint32_t pos = 0;
    for (int tries = 0; tries < 30; ++tries) {
      const int b = static_cast<int>(esp_random() % LibraryIndex::kBuckets);
      if (index_->bucketStart(v, b) < index_->bucketStart(v, b + 1)) {
        pos = index_->bucketStart(v, b);
        break;
      }
    }
    scroll_.jumpTo(static_cast<float>(pos) * kRowH);
    forceFrame_ = true;
    reflickAfterJump_ = true;
    nextActionMs_ = nowMs + 700;
    return;
  }
  if (actions_ % 5 == 0) {  // tracks: a jump to a random row
    scroll_.jumpTo(static_cast<float>(esp_random() % std::max<uint32_t>(1, count())) * kRowH);
    forceFrame_ = true;
    reflickAfterJump_ = true;
    nextActionMs_ = nowMs + 700;
    return;
  }
  if (off >= scroll_.maxOffset() - 1) {
    flingDir_ = -1;
  } else if (off <= 1) {
    flingDir_ = 1;
  } else if (actions_ % 3 == 0) {
    flingDir_ = -flingDir_;
  }
  scroll_.fling(nowMs, flingDir_ * flickPxPerS_);
  nextActionMs_ = nowMs + 700;  // flick again before the list stops: continuous motion
}

void ScrollLab::perSecond(uint32_t nowMs, bool force) {
  const uint32_t elapsed = nowMs - secStartMs_;
  if (elapsed < 1000 && !force) return;
  if (elapsed == 0) return;
  const uint32_t underruns = audio_.underrunsNow();
  const uint64_t busy = audio_.decodeBusyUsTotal();
  const float fps = frames_ * 1000.0f / elapsed;
  const float moving = std::min(1.0f, static_cast<float>(movingMs_) / elapsed);
  const float movingFps = movingMs_ ? frames_ * 1000.0f / movingMs_ : 0.0f;
  const float decode = static_cast<float>(busy - busy0_) / (elapsed * 1000.0f);
  const uint32_t du = underruns - underruns0_;
  if (frames_ > 0 || stress_) {
    const float f = frames_ ? static_cast<float>(frames_) : 1.0f;
    Serial.printf("[scroll] t=%lus fps=%.1f moving=%.0f%% fps_moving=%.1f held=%lums frame=%.1f/%.1fms draw=%.1fms "
                  "push=%.1fms rows_drawn=%lu slices=%lu lock=%.2f/%.2fms ring_min=%lums underruns=+%lu decode=%.0f%% "
                  "heap_min=%.1fK level=%s %s %s %s lines=%lu gap_max=%lums\n",
                  (unsigned long)((nowMs - (stress_ ? stressStartMs_ : secStartMs_)) / 1000), fps, moving * 100.0f,
                  movingFps, (unsigned long)heldMs_, frameUs_ / f / 1000.0f, frameMaxUs_ / 1000.0f, drawUs_ / f / 1000.0f, pushUs_ / f / 1000.0f,
                  (unsigned long)rowsDrawn_, (unsigned long)slicesPushed_, lock_.meanUs() / 1000.0f,
                  lock_.maxUs / 1000.0f, (unsigned long)(ringMin_ == UINT32_MAX ? 9999 : ringMin_), (unsigned long)du,
                  decode * 100.0f, heapMin_ / 1024.0f, ScrollGovernor::name(budget_.level),
                  !audio_.isPlaying() ? "stopped" : audio_.ringSteady() ? "steady" : "filling/draining",
                  KineticScroll::name(scroll_.phase()), hw() ? "hw" : "redraw", (unsigned long)linesPushed_,
                  (unsigned long)gapMaxMs_);
    // The frame rate in the tab bar's corner.
    char s[12];
    snprintf(s, sizeof(s), "%.0ffps", fps);
    LcdLock lock;
    auto& d = M5.Display;
    d.fillRect(kChipX + 2, 4, 36, 28, col::BTN);
    d.setFont(&fonts::Font0);
    d.setTextColor(col::TXT, col::BTN);
    d.setTextDatum(textdatum_t::middle_center);
    d.drawString(s, kChipX + 20, 12);
    d.setTextColor(budget_.level == ScrollGovernor::Level::Normal ? col::GREEN : col::AMBER, col::BTN);
    d.drawString(ScrollGovernor::name(budget_.level), kChipX + 20, 24);
    d.setTextDatum(textdatum_t::top_left);
  }
  if (stress_ && seconds_ && nSeconds_ < static_cast<uint32_t>(kMaxSeconds)) {
    seconds_[nSeconds_++] = {fps,
                             movingFps,
                             moving,
                             frameMaxUs_ / 1000.0f,
                             lock_.maxUs / 1000.0f,
                             static_cast<uint16_t>(ringMin_ == UINT32_MAX ? 65535 : std::min<uint32_t>(ringMin_, 65534)),
                             static_cast<uint16_t>(std::min<uint32_t>(du, 65535)),
                             decode,
                             heapMin_,
                             static_cast<float>(gapMaxMs_)};
  }
  secStartMs_ = nowMs;
  frames_ = rowsDrawn_ = slicesPushed_ = 0;
  frameUs_ = drawUs_ = pushUs_ = 0;
  frameMaxUs_ = 0;
  movingMs_ = heldMs_ = 0;
  linesPushed_ = gapMaxMs_ = 0;
  lock_.reset();
  ringMin_ = heapMin_ = UINT32_MAX;
  underruns0_ = underruns;
  busy0_ = busy;
}

void ScrollLab::finishStress() {
  if (!stress_) return;
  stress_ = false;
  scroll_.jumpTo(std::round(scroll_.offset() / kRowH) * kRowH);
  forceFrame_ = true;
  if (!seconds_ || nSeconds_ == 0) {
    Serial.println("[scroll] stress stopped before a full second");
    return;
  }
  auto* fps = static_cast<float*>(psramAlloc(3 * kMaxSeconds * sizeof(float)));
  if (!fps) return;
  float* dec = fps + kMaxSeconds;
  float* mov = dec + kMaxSeconds;
  float frameMax = 0, lockMax = 0, movingSum = 0, gapMax = 0;
  uint32_t ringMin = UINT32_MAX, heapMin = UINT32_MAX;
  for (uint32_t i = 0; i < nSeconds_; ++i) {
    fps[i] = seconds_[i].movingFps;
    mov[i] = seconds_[i].moving * 100.0f;
    movingSum += seconds_[i].moving;
    dec[i] = seconds_[i].decode * 100.0f;
    frameMax = std::max(frameMax, seconds_[i].frameMaxMs);
    lockMax = std::max(lockMax, seconds_[i].lockMaxMs);
    ringMin = std::min<uint32_t>(ringMin, seconds_[i].ringMinMs);
    heapMin = std::min(heapMin, seconds_[i].heapMin);
    gapMax = std::max(gapMax, seconds_[i].gapMaxMs);
  }
  const Percentiles f = Percentiles::of(fps, nSeconds_);
  const Percentiles d = Percentiles::of(dec, nSeconds_);
  const Percentiles m = Percentiles::of(mov, nSeconds_);
  psramFree(fps);
  snprintf(lastSummary_, sizeof(lastSummary_),
           "w%d %lus: moving %.0f%% of the time (p10 %.0f%%), fps while moving p10=%.1f p50=%.1f min=%.1f, frame max "
           "%.1fms, SPI hold max %.2fms, ring min %lums, underruns %lu, decode p50=%.0f%% max=%.0f%%, heap min %.1fK "
           "(ever %.1fK); %s, flicks at %.0f px/s, loop gap max %.0fms",
           mode_, (unsigned long)nSeconds_, movingSum * 100.0f / nSeconds_, m.p10, f.p10, f.p50, f.min, frameMax,
           lockMax, (unsigned long)ringMin,
           (unsigned long)(audio_.underrunsNow() - runUnderruns0_), d.p50, d.max, heapMin / 1024.0f,
           internalMinEver() / 1024.0f, kPathNames[static_cast<int>(path_)], flickPxPerS_, gapMax);
  Serial.printf("[scroll] stress done: %s\n", lastSummary_);
}

void ScrollLab::printSettings() {
  const RefillPacer::Config pace = audio_.refillPacing();
  Serial.printf("[scroll] settings: view %s, frame cap %lu ms, slices %d px, %d cached row sprite(s), stress %lu s at %.0f px/s, "
                "governor %s (w0); %s, flings capped at %.0f px/s, refill pacing %s at %.1fx from %lu ms (wp); index %s\n",
                kViewNames[view_], (unsigned long)normalFrameMs_, sliceH_, cacheRows_,
                (unsigned long)(stressMs_ / 1000), flickPxPerS_, governed_ ? "on" : "off", kPathNames[static_cast<int>(path_)],
                KineticScroll::Config{}.maxPxPerS, pace.enabled ? "on" : "off", pace.capX10 / 10.0f,
                (unsigned long)pace.gentleFromMs, index_ && index_->ready() ? "ready" : "none");
  if (lastSummary_[0]) Serial.printf("[scroll] last stress: %s\n", lastSummary_);
}

void ScrollLab::command(const char* arg, LibraryIndex* index) {
  if (!arg || !*arg) {
    if (active_) {
      close();
    } else {
      open(index, 0);
    }
    return;
  }
  const char c = arg[0];
  const int v = atoi(arg + 1);
  if (c >= '0' && c <= '3') {
    close();
    open(index, c - '0');
    return;
  }
  switch (c) {
    case 'q': close(); return;
    case 's': printSettings(); return;
    case 'f':
      normalFrameMs_ = v <= 0 ? 0 : static_cast<uint32_t>(1000 / std::min(v, 100));
      break;
    case 'h': sliceH_ = constrain(v, 1, kRowH); break;
    case 'c':
      cacheRows_ = constrain(v, 1, kMaxSlots);
      if (slots_) {
        for (int i = 0; i < kMaxSlots; ++i) slots_[i].row = -1;
      }
      break;
    case 'd': stressMs_ = static_cast<uint32_t>(constrain(v, 5, 300)) * 1000; break;
    case 'k': flickPxPerS_ = static_cast<float>(constrain(v, 200, 6000)); break;
    case 'g': governed_ = v != 0; break;
    case 'm': setPath(constrain(v, 0, 2)); break;
    case 'b':
      Serial.println("[scroll] the interaction boost was removed (it measured worse: docs/UI-SPIKE.md)");
      return;
    case 'p': {
      // wp0 off, wp1 on at the current cap, wp<x10> on at x10/10 times realtime.
      const RefillPacer::Config pace = audio_.refillPacing();
      if (v <= 0) {
        audio_.setRefillPacing(false, pace.capX10);
      } else {
        // At least RefillPacer::kMinCapX10 (1.5x): a cap near 1x would let the
        // ring settle at 500 ms (the backend clamps it too).
        audio_.setRefillPacing(
            true, v == 1 ? pace.capX10 : static_cast<uint32_t>(constrain(v, static_cast<int>(RefillPacer::kMinCapX10), 40)));
      }
      break;
    }
    case 'v':
      if (active_) {
        setView(static_cast<View>(constrain(v, 0, 2)));
        drawHeader();
        if (stress_ && scroll_.maxOffset() < kMinStressPx) {
          Serial.printf("[scroll] the %s list is too short for a stress (%.0f px): stopped\n", kViewNames[view_],
                        scroll_.maxOffset());
          finishStress();
        }
      } else {
        view_ = static_cast<View>(constrain(v, 0, 2));
      }
      break;
    default:
      Serial.println("[scroll] w toggles; w0 interactive, w1 stress, w2 stress without governor, w3 no frame cap; "
                     "wv<0-2> wf<fps> wh<px> wc<n> wd<s> wk<px/s> wg0/1; wm0 full redraw, wm1 hardware scroll (default); "
                     "wp0/wp1/wp<15-40> refill pacing; ws wq");
      return;
  }
  if (active_ && mode_ == 0) {
    ScrollGovernor::Config g = gov_.config();
    g.enabled = governed_;
    g.normalFrameMs = normalFrameMs_;
    gov_.setConfig(g);
  }
  printSettings();
}

void ScrollLab::loop(uint32_t nowMs) {
  if (!active_) return;
  // The caller's nowMs was read at the top of the main loop, before the
  // console ran; open() stamps its start times with millis() afterwards. So
  // the first pass after an open can be a few ms "before" the open: clamp,
  // or every elapsed-time difference below wraps (a stress ended at once).
  if (static_cast<int32_t>(nowMs - lastLoopMs_) < 0) nowMs = lastLoopMs_;
  const uint32_t ring = audio_.bufferedMsNow();
  // The ring's low point while it matters (see ringSteady()): a track change
  // empties it on purpose. 65535 in a second's record: never steady.
  if (audio_.isPlaying() && audio_.ringSteady() && ring < ringMin_) ringMin_ = ring;
  const uint32_t heap = internalFree();
  if (heap < heapMin_) heapMin_ = heap;

  if (stress_) stressStep(nowMs);
  // A low ring is only a risk once the track has filled it: not while it
  // fills at a track's start, nor while it drains at a file's end (a track
  // change took the list to "paused" for ~4 s before this).
  budget_ = gov_.update(nowMs, ring, audio_.underrunsNow(), audio_.isPlaying() && audio_.ringSteady());
  scroll_.update(nowMs);
  int off = static_cast<int>(std::lround(scroll_.offset()));
  if (budget_.wholeRows && scroll_.moving()) off = off / kRowH * kRowH;
  // Time the list wanted frames (moving, or a frame still owed), so an idle
  // screen and a throttled one read differently: fps counts frames drawn,
  // fps_moving frames per second of wanting them, held the part of that the
  // governor spent above normal.
  const bool wanted = scroll_.moving() || off != drawnOffset_ || forceFrame_;
  const uint32_t dt = nowMs - lastLoopMs_;
  lastLoopMs_ = nowMs;
  if (wanted) {
    movingMs_ += dt;
    if (budget_.level != ScrollGovernor::Level::Normal) heldMs_ += dt;
  }
  if (dt > gapMaxMs_) gapMaxMs_ = dt;
  pollTrackStart(nowMs, dt);
  // The rail: hidden while the list moves (it sits in the scrolled band),
  // back once it settles, or at once under a finger.
  railWanted_ = railWanted();
  if (!railWanted_ && railUp_) hideRail();
  // Frames on the cap's cadence: each one due at its deadline, the next a
  // period later, so a pass that comes late doesn't push every later frame
  // back; after a longer wait (idle, a stall) the cadence starts over.
  const uint32_t frameMs = budget_.frameMs;
  const bool due = frameMs == 0 || static_cast<int32_t>(nowMs - nextFrameMs_) >= 0;
  const bool owed = off != drawnOffset_ || forceFrame_;
  if (budget_.draw && due && owed) {
    const int64_t t0 = nowUs();
    if (hw()) {
      renderFrameHw(off);  // the rail with it, if it's wanted
    } else {
      renderFrame(off);
      if (railWanted_) {
        drawRail(off, forceFrame_ || !railUp_);
        railUp_ = true;
      }
    }
    const auto us = static_cast<uint32_t>(nowUs() - t0);
    frameUs_ += us;
    if (us > frameMaxUs_) frameMaxUs_ = us;
    lastFrameUs_ = us;
    if (watching_) {
      ++watchFrames_;
      if (us > watchFrameMaxUs_) watchFrameMaxUs_ = us;
    }
    ++frames_;
    nextFrameMs_ += frameMs;
    if (static_cast<int32_t>(nowMs - nextFrameMs_) >= 0) nextFrameMs_ = nowMs + frameMs;
    lastFrameEndMs_ = millis();
    drawnOffset_ = off;
    forceFrame_ = false;
  } else if (railWanted_ && !railUp_ && !owed) {
    showRail(drawnOffset_);  // the list came to rest (or a finger is on the rail)
  }
  perSecond(nowMs, false);
}
