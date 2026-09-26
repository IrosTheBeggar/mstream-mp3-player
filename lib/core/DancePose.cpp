#include "DancePose.h"

#include <cmath>

namespace dance {
namespace {
constexpr float kPi = 3.14159265f;
constexpr float kDeg = kPi / 180.0f;

constexpr float kStance = 14.0f;       // feet this far either side of the centre
constexpr float kHipHalf = 6.0f;       // hip joints either side of the hip centre
constexpr float kShoulderHalf = 9.0f;    // wide enough that a raised arm clears the head
constexpr float kStandHip = 46.0f;     // hip height standing, knees soft
constexpr float kSquashPx = 7.0f;      // the body sinks this much on impact
constexpr float kSquashPeak = 0.03f;   // ... deepest this far into the beat
constexpr float kSquashBeats = 0.2f;   // ... and recovered by here
constexpr float kStretchFrom = 0.85f;  // anticipation stretch from here to the beat
constexpr float kStretchPx = 3.0f;
constexpr float kHeadBobPx = 3.5f;
constexpr float kHeadLag = 0.1f;       // beats after the impact the head is lowest
constexpr float kSwayPx = 3.0f;
constexpr float kIdleHz = 0.25f;
constexpr float kIdleHip = 47.5f;     // idle stands taller (feet still reach the ground mid-sway)

Point add(Point a, float dx, float dy) { return {a.x + dx, a.y + dy}; }
float lerp(float a, float b, float t) { return a + (b - a) * t; }
Point lerp(Point a, Point b, float t) { return {lerp(a.x, b.x, t), lerp(a.y, b.y, t)}; }

// The foot at `target`, or as close as a straight leg from `hip` reaches:
// at the top of a hop the feet leave the ground.
Point reach(Point hip, Point target) {
  const float dx = target.x - hip.x, dy = target.y - hip.y;
  const float d = std::sqrt(dx * dx + dy * dy);
  const float leg = kThigh + kShin;
  if (d <= leg || d <= 0.0f) return target;
  return {hip.x + dx * leg / d, hip.y + dy * leg / d};
}

// An arm from `shoulder`: `raise` 0 hangs down, 1 is up over the head.
// side +1 is the screen-right arm, -1 the screen-left one.
void arm(Point shoulder, float raise, float side, float baseDeg, Point* elbow, Point* hand) {
  const float up = (baseDeg + 125.0f * raise) * kDeg;       // from straight down, outwards
  const float bend = up + (20.0f + 10.0f * raise) * kDeg;   // raised: the fist straight up
  *elbow = add(shoulder, side * kUpperArm * std::sin(up), kUpperArm * std::cos(up));
  *hand = add(*elbow, side * kForearm * std::sin(bend), kForearm * std::cos(bend));
}

// Hips, feet and legs for a hip centre; the rest hangs off the neck.
void legs(Pose& p, const Box& box) {
  const float cx = box.w / 2.0f;
  const Point hipL = add(p.hip, kHipHalf, 0.0f), hipR = add(p.hip, -kHipHalf, 0.0f);
  p.footL = reach(hipL, {cx + kStance, box.groundY});
  p.footR = reach(hipR, {cx - kStance, box.groundY});
  p.kneeL = knee(hipL, p.footL, +1.0f);
  p.kneeR = knee(hipR, p.footR, -1.0f);
}
}  // namespace

Point knee(Point hip, Point foot, float sign) {
  const float dx = foot.x - hip.x, dy = foot.y - hip.y;
  const float d = std::sqrt(dx * dx + dy * dy);
  if (d <= 1e-3f) return add(hip, sign * kThigh, 0.0f);
  if (d >= kThigh + kShin) return {hip.x + dx * kThigh / d, hip.y + dy * kThigh / d};
  // Equal bones: the knee sits on the perpendicular through the midpoint.
  const float h = std::sqrt(kThigh * kThigh - d * d / 4.0f);
  float px = -dy / d, py = dx / d;
  if (px * sign < 0.0f) {
    px = -px;
    py = -py;
  }
  return {(hip.x + foot.x) / 2.0f + px * h, (hip.y + foot.y) / 2.0f + py * h};
}

Pose dancePose(float phi, bool odd, const Box& box) {
  phi -= std::floor(phi);
  const float cx = box.w / 2.0f;
  const float hop = kHopPx * std::sin(kPi * phi);
  // Squash: in over the first few hundredths of the beat (under a frame at
  // 30 fps), out by kSquashBeats. Stretch: up and back down before the beat.
  // Both start and end at 0, so the pose is continuous across beats.
  float squash = 0.0f;
  if (phi < kSquashPeak) {
    squash = phi / kSquashPeak;
  } else if (phi < kSquashBeats) {
    const float s = (kSquashBeats - phi) / (kSquashBeats - kSquashPeak);
    squash = s * s;
  }
  const float stretch = phi > kStretchFrom ? std::sin(kPi * (phi - kStretchFrom) / (1.0f - kStretchFrom)) : 0.0f;
  // +1: the screen-right arm is up. Extreme at phi = 0, flips each beat, and
  // is continuous across it (cos(pi) of one beat is -cos(0) of the next).
  const float swing = (odd ? -1.0f : 1.0f) * std::cos(kPi * phi);

  Pose p;
  p.hip = {cx + kSwayPx * swing, box.groundY - kStandHip - hop + kSquashPx * squash - kStretchPx * stretch};
  const float torso = kTorso - 3.0f * squash + 2.0f * stretch;
  p.neck = {p.hip.x + 1.5f * swing, p.hip.y - torso};
  const float bob = 0.5f + 0.5f * std::cos(2.0f * kPi * (phi - kHeadLag));
  p.head = {p.neck.x + 0.8f * swing, p.neck.y - kNeck + kHeadBobPx * bob - 1.0f};
  p.shoulderL = add(p.neck, kShoulderHalf, 4.0f);
  p.shoulderR = add(p.neck, -kShoulderHalf, 4.0f);
  arm(p.shoulderL, (1.0f + swing) / 2.0f, +1.0f, 25.0f, &p.elbowL, &p.handL);
  arm(p.shoulderR, (1.0f - swing) / 2.0f, -1.0f, 25.0f, &p.elbowR, &p.handR);
  legs(p, box);
  return p;
}

Pose idlePose(float seconds, const Box& box) {
  const float cx = box.w / 2.0f;
  const float s = std::sin(2.0f * kPi * kIdleHz * seconds);
  const float breath = std::sin(2.0f * kPi * 2.0f * kIdleHz * seconds);
  Pose p;
  p.hip = {cx + kSwayPx * s, box.groundY - kIdleHip + 0.8f * breath};
  p.neck = {p.hip.x + 1.5f * s, p.hip.y - kTorso};
  p.head = {p.neck.x + 1.0f * s, p.neck.y - kNeck};
  p.shoulderL = add(p.neck, kShoulderHalf, 4.0f);
  p.shoulderR = add(p.neck, -kShoulderHalf, 4.0f);
  arm(p.shoulderL, 0.0f, +1.0f, 12.0f + 4.0f * s, &p.elbowL, &p.handL);
  arm(p.shoulderR, 0.0f, -1.0f, 12.0f - 4.0f * s, &p.elbowR, &p.handR);
  legs(p, box);
  return p;
}

Pose blend(const Pose& a, const Pose& b, float t) {
  Pose p;
  p.head = lerp(a.head, b.head, t);
  p.headR = lerp(a.headR, b.headR, t);
  p.neck = lerp(a.neck, b.neck, t);
  p.hip = lerp(a.hip, b.hip, t);
  p.shoulderL = lerp(a.shoulderL, b.shoulderL, t);
  p.elbowL = lerp(a.elbowL, b.elbowL, t);
  p.handL = lerp(a.handL, b.handL, t);
  p.shoulderR = lerp(a.shoulderR, b.shoulderR, t);
  p.elbowR = lerp(a.elbowR, b.elbowR, t);
  p.handR = lerp(a.handR, b.handR, t);
  p.kneeL = lerp(a.kneeL, b.kneeL, t);
  p.footL = lerp(a.footL, b.footL, t);
  p.kneeR = lerp(a.kneeR, b.kneeR, t);
  p.footR = lerp(a.footR, b.footR, t);
  return p;
}

namespace {
constexpr double kFoldLo = 80.0, kFoldHi = 160.0;  // a fresh fold lands here
constexpr double kKeepLo = 72.0, kKeepHi = 176.0;  // a chosen one stays while in here

double freshScale(float bpm) {
  double scale = 1.0;
  if (bpm > 0.0f) {
    while (bpm * scale > kFoldHi) scale /= 2.0;
    while (bpm * scale < kFoldLo) scale *= 2.0;
  }
  return scale;
}
}  // namespace

double TempoFold::apply(float bpm) {
  if (bpm <= 0.0f) return scale();
  const double folded = bpm * scale_;
  if (scale_ <= 0.0 || folded < kKeepLo || folded > kKeepHi) scale_ = freshScale(bpm);
  return scale_;
}

Step danceStep(double beats, float bpm) { return danceStep(beats, bpm, freshScale(bpm)); }

Step danceStep(double beats, float bpm, double scale) {
  const double d = beats * scale;
  const double whole = std::floor(d);
  Step s;
  s.index = static_cast<int32_t>(static_cast<int64_t>(whole));
  s.phi = static_cast<float>(d - whole);
  s.odd = (s.index & 1) != 0;
  s.bpm = static_cast<float>(bpm * scale);
  return s;
}

float danceWeight(float confidence) {
  float x = (confidence - 0.3f) / 0.4f;
  if (x < 0.0f) x = 0.0f;
  if (x > 1.0f) x = 1.0f;
  return x * x * (3.0f - 2.0f * x);
}

Pose Dancer::update(float phi, bool odd, float confidence, bool beat, float dt) {
  if (dt < 0.0f) dt = 0.0f;
  const float target = beat ? danceWeight(confidence) : 0.0f;
  weight_ += (target - weight_) * (1.0f - std::exp(-dt / 0.4f));
  idle_ += dt;
  if (idle_ > 400.0f) idle_ -= 400.0f;  // 100 idle periods: seamless
  if (beat) {
    lastPhi_ = phi;
    lastOdd_ = odd;
  }
  // Without a beat the dance half holds its last pose while it fades out.
  return blend(idlePose(idle_, box_), dancePose(lastPhi_, lastOdd_, box_), weight_);
}

}  // namespace dance
