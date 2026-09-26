#pragma once
#include <cstdint>

// The dancing stick figure's joints, as pure functions of where the music is
// in the beat. Screen coordinates in the figure's own box: x right, y down,
// the ground line at groundY. Nothing here draws; ui/DanceView does.
//
// Motion follows how people dance to a beat (Takehana et al. 2019): ground
// contact and the extreme poses land ON the beat.
//   - A hop, height A * sin(pi * phi): the feet touch down at phi = 0.
//   - Squash on impact (knees bend, the body sinks) just after phi = 0, and
//     an anticipation stretch (legs straight, body long) at phi 0.85-1.
//   - The head bobs down about 0.1 beat after the impact.
//   - The arms alternate each beat, each reaching its extreme on the beat;
//     the hips sway with them.
// With no beat to follow (low confidence) the figure blends into an idle
// sway, so an intro or a breakdown doesn't leave it hopping at random.
namespace dance {

struct Point {
  float x = 0.0f;
  float y = 0.0f;
};

struct Box {
  float w = 120.0f;
  float h = 150.0f;
  float groundY = 142.0f;
};

struct Pose {
  Point head;  // centre
  float headR = 11.0f;
  Point neck, hip;
  Point shoulderL, elbowL, handL;  // L: the figure's own left, on screen right (it faces us)
  Point shoulderR, elbowR, handR;
  Point kneeL, footL, kneeR, footR;
};

// Bone lengths (px), for the tests and the renderer's line widths.
constexpr float kThigh = 25.0f;
constexpr float kShin = 25.0f;
constexpr float kTorso = 36.0f;
constexpr float kNeck = 13.0f;   // neck to head centre
constexpr float kUpperArm = 20.0f;
constexpr float kForearm = 18.0f;
constexpr float kHopPx = 10.5f;  // hop height at phi = 0.5

// On the beat: phi in [0, 1) since the last beat, `odd` that beat's parity.
Pose dancePose(float phi, bool odd, const Box& box = Box());
// Idle sway, `seconds` into it (it loops every few seconds).
Pose idlePose(float seconds, const Box& box = Box());
// Joint by joint: a at t = 0, b at t = 1.
Pose blend(const Pose& a, const Pose& b, float t);
// How much of the dance to show for a tracker confidence: 0 below 0.3,
// 1 above 0.7, smooth between.
float danceWeight(float confidence);
// A leg with the hip and foot given: the knee, bending outwards (sign -1 to
// the left of the screen, +1 to the right). Thigh and shin keep their length.
Point knee(Point hip, Point foot, float sign);

// Where the figure is in its own beat. It dances at the tracked tempo folded
// into 80-160 BPM: a 174 BPM track at 87, 70 BPM at 140.
struct Step {
  float phi = 0.0f;   // 0..1 since the last dance beat
  bool odd = false;   // that beat's parity
  int32_t index = 0;  // dance beats counted
  float bpm = 0.0f;   // the dance tempo
};
// `beats`: the tracker's beat count at the moment shown (whole beats plus
// the fraction since the last), at `bpm`. Folded from scratch each call.
Step danceStep(double beats, float bpm);
// The same with the fold chosen by a TempoFold (below): beats * scale.
Step danceStep(double beats, float bpm, double scale);

// The fold, kept from frame to frame. The tracker's tempo moves a little on
// every beat, so a track right at 160 or 80 BPM would cross the fold's edge
// and back each beat or two, and the figure would jump between hopping on
// every beat and on every other one. The scale chosen stays while the folded
// tempo is within 72-176 BPM (10 % past each edge) and is chosen afresh,
// into 80-160, only when it leaves that band.
class TempoFold {
public:
  // The scale to dance `bpm` at: 1, 1/2, 2, ... (1 for no tempo).
  double apply(float bpm);
  double scale() const { return scale_ > 0.0 ? scale_ : 1.0; }
  void reset() { scale_ = 0.0; }

private:
  double scale_ = 0.0;  // 0: none chosen yet
};

// The state around the pure functions: the idle clock and a smoothed dance
// weight (confidence changes and lost beats fade over ~0.4 s, never jump).
class Dancer {
public:
  explicit Dancer(const Box& box = Box()) : box_(box) {}
  // One frame, dt seconds after the last. `beat`: phi/odd are meaningful.
  Pose update(float phi, bool odd, float confidence, bool beat, float dt);
  // A fixed pose for screenshots: full dance weight, no smoothing.
  Pose frozen(float phi, bool odd) const { return dancePose(phi, odd, box_); }
  float weight() const { return weight_; }
  // `weight`: start there (a skin switch hands over the other skin's weight).
  void reset(float weight = 0.0f) {
    weight_ = weight < 0.0f ? 0.0f : (weight > 1.0f ? 1.0f : weight);
    idle_ = 0.0f;
  }

private:
  Box box_;
  float weight_ = 0.0f;
  float idle_ = 0.0f;  // seconds of idle clock
  float lastPhi_ = 0.0f;
  bool lastOdd_ = false;
};

}  // namespace dance
