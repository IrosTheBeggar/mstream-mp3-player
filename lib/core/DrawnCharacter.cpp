// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "DrawnCharacter.h"

namespace dance {

static_assert(sizeof(DrawnCharacter) <= 6144, "E.1: step 4 psramNews it; at most 6 KB");

namespace {

constexpr int64_t kQ16 = 65536;
constexpr int64_t kBeatQ16 = 12 * kQ16;  // 786432: one move beat in Q16 ticks

int clampSpot(int spot) { return spot < -1 ? -1 : (spot > 1 ? 1 : spot); }

// B.0's rhu(a, 2^k): (a + 2^(k-1)) >> k, a floor division on int64_t (an
// arithmetic shift), so a negative a rounds half up too.
int64_t rhuShift(int64_t a, int k) { return (a + (int64_t{1} << (k - 1))) >> k; }

int16_t i16(int64_t v) { return static_cast<int16_t>(v); }

// a // b for a >= 0 and b > 0: one 32-bit division when both fit (a single
// QUOU on the ESP32 instead of libgcc's __divdi3), else the 64-bit one. Equal
// results either way.
inline int64_t udiv(int64_t a, int64_t b) {
  if (((static_cast<uint64_t>(a) | static_cast<uint64_t>(b)) >> 32) == 0)
    return static_cast<uint32_t>(a) / static_cast<uint32_t>(b);
  return a / b;
}

StageRect rect(int64_t x0, int64_t y0, int64_t x1, int64_t y1) {
  StageRect r;
  r.x0 = i16(x0);
  r.y0 = i16(y0);
  r.x1 = i16(x1);
  r.y1 = i16(y1);
  return r;
}

// A.13: a step travels when it has `go`, or when no step has.
bool inGo(const MoveSteps& m, int k) { return !m.anyGo || m.step[k].go; }

// The starts and the travelling sums of m's first `count` steps.
void sums(MoveSteps* m) {
  m->anyGo = false;
  for (int k = 0; k < m->count; ++k) m->anyGo = m->anyGo || m->step[k].go;
  int32_t total = 0, go = 0;
  for (int k = 0; k < m->count; ++k) {
    m->start[k] = total;
    m->goStart[k] = go;
    total += m->step[k].dur;
    if (inGo(*m, k)) go += m->step[k].dur;
  }
  m->total = total;
  m->goTotal = go;
}

// B.4's "any other record": frame 0, unmirrored, at the spot, no box or shadow.
Placement unreadable(int spot) {
  Placement p = {};
  p.rootX = i16(spotX(spot));
  p.rootY = kStage.groundY;
  return p;
}

// The engine moves (E.1): B.4 on two synthetic records on GEOM's rest frame.
void engineMove(const Pack& pack, int which, MoveSteps* m) {
  *m = MoveSteps();
  const uint16_t rest = pack.geom().restFrame;
  StepRecord s = {};
  s.frame = static_cast<uint16_t>(rest & 0x7FFF);
  s.mirrored = (rest & kFrameMirrored) != 0;
  s.ease = 0;
  s.go = false;
  m->ok = true;
  if (which == kAutoBounce) {  // 2.8: one beat, unranged, a hop of 4 native px, one held step
    m->kind = 0;
    m->beats = 1;
    m->bounce = 4;
    s.dur = 12;
    m->step[0] = s;
    m->count = 1;
  } else {  // 2.6: the rest frame swaying +-1 native px over 4 s
    m->kind = 1;
    s.dur = 200;
    s.ease = 4;
    s.dx = -1;
    m->step[0] = s;
    s.dx = 1;
    m->step[1] = s;
    m->count = 2;
  }
  sums(m);  // L 12 or 400
}

}  // namespace

// ---- B.2, B.3 --------------------------------------------------------------

int32_t ease16(int ease, int32_t f) {
  const int64_t x = f;
  switch (ease) {
    case 1: return f;
    case 2: return static_cast<int32_t>((x * x) >> 16);
    case 3: {
      const int64_t g = kQ16 - x;
      return static_cast<int32_t>(kQ16 - ((g * g) >> 16));
    }
    case 4: return static_cast<int32_t>((x * x * (196608 - 2 * x)) >> 32);
    default: return f >= 65536 ? 65536 : 0;  // 0 hold, and any code above 4
  }
}

int32_t hop16(int32_t phi) {
  if (phi < 7864) return 0;
  const int64_t a = udiv((static_cast<int64_t>(phi) - 7864) * kQ16, 57672);  // under 2^32 for phi < 65536
  return static_cast<int32_t>((4 * a * (kQ16 - a)) >> 16);
}

int32_t sway16(int64_t b, int32_t phi) {
  const int32_t s = (b & 1) ? 1 : -1;  // b mod 2, as Python's (also for a negative b)
  return s * (65536 - 2 * ease16(4, phi));
}

int stageX(int from, int to, int32_t p16, int ease) {
  const int x0 = spotX(clampSpot(from)), x1 = spotX(clampSpot(to));
  return x0 + static_cast<int>(rhuShift(static_cast<int64_t>(x1 - x0) * ease16(ease, p16), 16));
}

// ---- B.4 -------------------------------------------------------------------

bool readMoveSteps(const Pack& pack, int move, MoveSteps* out) {
  *out = MoveSteps();
  // One record decode for all the steps; steps past STEP's end read as zeros
  // (Pack::step's rule), so their sum is 0.
  MoveRecord r;
  const int n = pack.steps(move, out->step, kMaxSteps, &r);
  if (r.stepCount < 1 || r.stepCount > kMaxSteps || n != r.stepCount) {
    *out = MoveSteps();
    return false;
  }
  out->count = r.stepCount;
  sums(out);
  if (out->total <= 0) {
    *out = MoveSteps();
    return false;
  }
  out->ok = true;
  out->kind = r.kind;
  out->beats = r.beats;
  out->flags = r.flags;
  out->bounce = r.bounce;
  out->sway = r.sway;
  out->travelEase = r.travelEase;
  out->startAt = r.startAt;
  out->travel = r.travel;
  return true;
}

Placement placeSteps(const Pack& pack, const MoveSteps& m, int64_t pos16, bool mirrored, int spot,
                     int32_t e16) {
  spot = clampSpot(spot);
  if (!m.ok || m.count < 1 || m.total <= 0) return unreadable(spot);
  if (pos16 < 0) pos16 = 0;
  const PackGeom& g = pack.geom();
  const int n = m.count;

  // 1-2. The wrap and the step: the last k with s_k * 65536 <= q, that is
  // s_k <= q >> 16 (the starts never decrease), by binary search.
  const int64_t period = static_cast<int64_t>(m.total) * kQ16;
  const int64_t loop = udiv(pos16, period);
  const int64_t q = pos16 - loop * period;
  const int64_t whole = q >> 16;
  int i = 0, hi = n - 1;
  while (i < hi) {
    const int mid = (i + hi + 1) >> 1;
    if (m.start[mid] <= whole) i = mid;
    else hi = mid - 1;
  }
  const StepRecord& st = m.step[i];
  const int64_t into = q - static_cast<int64_t>(m.start[i]) * kQ16;
  const int32_t f = st.dur > 0 ? static_cast<int32_t>(udiv(into, st.dur)) : 0;  // the chosen step's dur is > 0

  // 3. The frame.
  const bool drawn = st.mirrored != mirrored;

  // 4. The key, easing into the next step's (the last into the first).
  const StepRecord& nx = m.step[(i + 1) % n];
  const int64_t e = ease16(st.ease, f);
  const int64_t kx = static_cast<int64_t>(st.dx) * kQ16 + (static_cast<int64_t>(nx.dx) - st.dx) * e;
  const int64_t ky = static_cast<int64_t>(st.dy) * kQ16 + (static_cast<int64_t>(nx.dy) - st.dy) * e;

  // 5. Bounce and sway (beat moves), from the unwrapped pos16.
  int64_t by = 0, sx = 0;
  if (m.kind == 0) {
    const int64_t b = udiv(pos16, kBeatQ16);
    const int32_t phi = static_cast<int32_t>(udiv(pos16 - b * kBeatQ16, 12));
    by = -static_cast<int64_t>(m.bounce) * hop16(phi);
    sx = static_cast<int64_t>(m.sway) * sway16(b, phi);
  }

  // 6-7. The orientation, then screen px, rounded half up.
  const int64_t sgn = mirrored ? -1 : 1;
  const int64_t x16 = sgn * (kx + sx);
  const int64_t y16 = ky + by;
  const int64_t scale = g.scale;
  int64_t X, Y;
  if (m.flags & kSnap) {
    X = scale * rhuShift(static_cast<int64_t>(e16) * x16, 32);
    Y = scale * rhuShift(static_cast<int64_t>(e16) * y16, 32);
  } else {
    X = rhuShift(scale * e16 * x16, 32);
    Y = rhuShift(scale * e16 * y16, 32);
  }

  // 8. Travel: the start spot to the end spot over the `go` steps.
  const int64_t startX = spotX(spot);
  int64_t travelPx = 0;
  if (m.kind == 0 && m.travel != 0) {
    const int endSpot = clampSpot(spot + static_cast<int>(m.travel * sgn));
    int32_t p = 65536;  // done: a later play, or no `go` time at all
    if (loop < 1 && m.goTotal > 0) {
      int64_t done = static_cast<int64_t>(m.goStart[i]) * kQ16;  // the go steps before i, whole
      if (inGo(m, i)) done += into;
      p = static_cast<int32_t>(udiv(done, m.goTotal));
    }
    travelPx = stageX(spot, endSpot, p, m.travelEase) - startX;
  }

  // 9. The root.
  int64_t rootX = startX + travelPx + X;
  int64_t rootY = kStage.groundY + Y;

  // 10-11. The box, clamped onto the stage (a blank frame has none).
  Placement out = {};
  out.frame = st.frame;
  out.mirrored = drawn;
  const FrameBox b = pack.box(st.frame);
  if (b.w > 0 && b.h > 0) {
    const int64_t left = drawn ? rootX + (static_cast<int64_t>(g.anchorX) - b.x0 - b.w + 1) * scale
                               : rootX + (static_cast<int64_t>(b.x0) - g.anchorX) * scale;
    const int64_t top = rootY + (static_cast<int64_t>(b.y0) - g.anchorY) * scale;
    const int64_t right = left + b.w * scale, bottom = top + b.h * scale;
    const int64_t cx = left < 0 ? -left : (right > kStage.w ? kStage.w - right : 0);
    const int64_t cy = top < 0 ? -top : (bottom > kStage.h ? kStage.h - bottom : 0);
    rootX += cx;
    rootY += cy;
    out.box = rect(left + cx, top + cy, right + cx, bottom + cy);
  }

  // 12. The shadow (B.5): 2 px narrower per px of hop, Y taken before the clamp.
  const int64_t hop = Y < 0 ? -Y : 0;
  const int64_t sw = static_cast<int64_t>(g.shadowW) - 2 * hop;
  if (sw > 0) {
    const int64_t x0 = rootX - sw / 2;
    const int64_t l = x0 > 0 ? x0 : 0;
    const int64_t r = x0 + sw < kStage.w ? x0 + sw : kStage.w;
    const StageRect s = rect(l, kStage.groundY - 1, r, kStage.groundY + 2);
    if (!rectEmpty(s)) out.shadow = s;
  }
  out.rootX = i16(rootX);
  out.rootY = i16(rootY);
  out.rect = rectUnion(out.box, out.shadow);
  return out;
}

Placement frameAt(const Pack& pack, int move, int64_t pos16, bool mirrored, int spot, int32_t e16) {
  MoveSteps m;
  readMoveSteps(pack, move, &m);
  return placeSteps(pack, m, pos16, mirrored, spot, e16);
}

// ---- B.11 ------------------------------------------------------------------

namespace {

bool extentOf(const Pack& pack, const MoveSteps& m, bool mirrored, int64_t* lo, int64_t* hi) {
  if (!m.ok) return false;
  const PackGeom& g = pack.geom();
  const int64_t sgn = mirrored ? -1 : 1;
  const int64_t scale = g.scale;
  const int64_t sway = m.kind == 0 ? m.sway : 0;
  bool any = false;
  for (int i = 0; i < m.count; ++i) {
    const StepRecord& st = m.step[i];
    const FrameBox b = pack.box(st.frame);
    if (b.w == 0 || b.h == 0) continue;
    const bool drawn = st.mirrored != mirrored;
    const int64_t left = drawn ? (static_cast<int64_t>(g.anchorX) - b.x0 - b.w + 1) * scale
                               : (static_cast<int64_t>(b.x0) - g.anchorX) * scale;
    const int64_t right = left + b.w * scale;
    const int64_t a = sgn * st.dx * scale;
    const int64_t c = sgn * m.step[(i + 1) % m.count].dx * scale;
    int64_t mn = a < c ? a : c, mx = a > c ? a : c;
    if (mn > 0) mn = 0;
    if (mx < 0) mx = 0;
    const int64_t l = left + mn - sway * scale, h = right + mx + sway * scale;
    if (!any || l < *lo) *lo = l;
    if (!any || h > *hi) *hi = h;
    any = true;
  }
  return any;
}

bool fitsAt(bool hasExtent, int64_t lo, int64_t hi, int spot) {
  if (!hasExtent) return true;
  const int64_t x = spotX(spot);
  return x + lo >= 0 && x + hi <= kStage.w;
}

uint8_t spotsOf(const Pack& pack, const MoveSteps& m, bool mirrored) {
  if (!m.ok) return 0b111;
  const int sgn = mirrored ? -1 : 1;
  const int t = m.kind == 0 ? m.travel * sgn : 0;
  if (t == 0) return 0b111;
  const uint8_t allowed = startAtMask(static_cast<StartAt>(m.startAt), mirrored);
  int64_t lo = 0, hi = 0;
  const bool has = extentOf(pack, m, mirrored, &lo, &hi);
  uint8_t mask = 0;
  for (int s = -1; s <= 1; ++s) {
    const int to = s + t;
    if (to < -1 || to > 1 || !(allowed & (1u << (s + 1)))) continue;
    if (fitsAt(has, lo, hi, s) && fitsAt(has, lo, hi, to)) mask |= static_cast<uint8_t>(1u << (s + 1));
  }
  return mask;
}

}  // namespace

bool extent(const Pack& pack, int move, bool mirrored, int* lo, int* hi) {
  MoveSteps m;
  readMoveSteps(pack, move, &m);
  int64_t l = 0, h = 0;
  if (!extentOf(pack, m, mirrored, &l, &h)) return false;
  if (lo) *lo = static_cast<int>(l);
  if (hi) *hi = static_cast<int>(h);
  return true;
}

bool fits(const Pack& pack, int move, bool mirrored, int spot) {
  MoveSteps m;
  readMoveSteps(pack, move, &m);
  int64_t lo = 0, hi = 0;
  const bool has = extentOf(pack, m, mirrored, &lo, &hi);
  return fitsAt(has, lo, hi, clampSpot(spot));
}

uint8_t startSpotMask(const Pack& pack, int move, bool mirrored) {
  MoveSteps m;
  readMoveSteps(pack, move, &m);
  return spotsOf(pack, m, mirrored);
}

uint8_t startSpotMask(const Pack& pack, const MoveSteps& m, bool mirrored) { return spotsOf(pack, m, mirrored); }

// ---- DrawnCharacter ------------------------------------------------------------

namespace {
constexpr MoveLabels kNoLabels = {};
}  // namespace

DrawnCharacter::DrawnCharacter() : info_{"", "", "", "", "", Origin::BuiltIn, false, 0}, packMove_{}, spots_{},
                                   labels_{}, keys_{} {}

DrawnCharacter::DrawnCharacter(const DrawnCharacter& other) : DrawnCharacter() { *this = other; }

DrawnCharacter& DrawnCharacter::operator=(const DrawnCharacter& other) {
  if (this == &other) return *this;
  pack_ = other.pack_;
  info_ = other.info_;
  count_ = other.count_;
  for (int i = 0; i < kMaxMoves; ++i) {
    packMove_[i] = other.packMove_[i];
    spots_[i] = other.spots_[i];
    labels_[i] = other.labels_[i];
    if (other.labels_[i].genreKeys) labels_[i].genreKeys = keys_ + (other.labels_[i].genreKeys - other.keys_);
  }
  for (int k = 0; k < kMaxMoves * kMaxGenreTags; ++k) keys_[k] = other.keys_[k];
  return *this;
}

bool DrawnCharacter::init(const Pack& pack, Origin origin, bool inCycle) {
  // Cleared in place: a whole DrawnCharacter (~5 KB) is too big a temporary for the
  // loop task's stack. Entries past count_ are never read.
  pack_.clear();
  info_ = CharacterInfo{"", "", "", "", "", Origin::BuiltIn, false, 0};
  count_ = 0;
  if (pack.state() != Pack::State::Open) return false;
  pack_ = pack;
  info_.id = pack.id();
  info_.name = pack.name();
  info_.author = pack.author();
  info_.licence = pack.licence();
  info_.url = pack.url() ? pack.url() : "";
  info_.origin = origin;
  info_.inCycle = inCycle;
  info_.ignoredMoves = static_cast<uint8_t>(pack.moveCount() - pack.keptMoves());
  int keys = 0;
  for (int m = 0; m < pack.moveCount() && count_ < kMaxMoves; ++m) {
    if (!pack.moveKept(m)) continue;
    const MoveRecord r = pack.moveRecord(m);
    MoveSteps steps;
    readMoveSteps(pack, m, &steps);
    MoveLabels& l = labels_[count_];
    l = MoveLabels();
    l.name = pack.moveName(m);
    l.kind = r.kind == 1 ? MoveKind::Idle : MoveKind::Beat;
    l.beats = r.kind == 1 ? 0 : r.beats;
    l.idleMs = r.kind == 1 ? static_cast<uint16_t>(10 * steps.total) : 0;
    l.paceLo = r.paceLo;
    l.paceHi = r.paceHi;
    l.weight10 = r.weight10;
    l.when = r.when;
    l.flags = r.flags;
    l.repeat = r.repeat;
    l.fromTag = r.fromKey;
    l.toTag = r.toKey;
    l.fromTagM = r.fromKeyM;
    l.toTagM = r.toKeyM;
    l.needs = r.needs;
    l.travel = r.travel;
    l.startAt = static_cast<StartAt>(r.startAt);
    const int first = keys;
    for (int t = 0; t < r.genreCount && t < kMaxGenreTags; ++t) {
      uint32_t key = 0;
      const char* tag = pack.genreTag(m, t);
      if (tag && genreKey(tag, &key)) keys_[keys++] = key;  // always, for a kept move
    }
    l.genreCount = static_cast<uint8_t>(keys - first);
    l.genreKeys = keys > first ? keys_ + first : nullptr;
    spots_[count_] = static_cast<uint8_t>(spotsOf(pack, steps, false) | (spotsOf(pack, steps, true) << 4));
    packMove_[count_] = static_cast<int8_t>(m);
    ++count_;
  }
  return true;
}

const CharacterInfo& DrawnCharacter::info() const { return info_; }
int DrawnCharacter::moveCount() const { return count_; }
const MoveLabels& DrawnCharacter::move(int i) const { return i >= 0 && i < count_ ? labels_[i] : kNoLabels; }

uint8_t DrawnCharacter::startSpotMask(int i, bool mirrored) const {
  if (i < 0 || i >= count_) return 0;
  return static_cast<uint8_t>((spots_[i] >> (mirrored ? 4 : 0)) & 0b111);
}

int DrawnCharacter::packMove(int i) const { return i >= 0 && i < count_ ? packMove_[i] : -1; }
const Pack& DrawnCharacter::pack() const { return pack_; }

Placement DrawnCharacter::frameAt(int i, int64_t pos16, bool mirrored, int spot, int32_t e16) const {
  MoveSteps m;
  if (i >= 0 && i < count_) readMoveSteps(pack_, packMove_[i], &m);
  else if (i == kAutoBounce || i == kAutoIdle) engineMove(pack_, i, &m);
  return placeSteps(pack_, m, pos16, mirrored, spot, e16);
}

}  // namespace dance
