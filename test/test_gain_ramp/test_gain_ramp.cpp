// Host unit tests for VolumeMath and GainRamp. Run: pio test -e native
#include <unity.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "GainRamp.h"
#include "VolumeMath.h"

namespace {
constexpr int16_t kProbe = 32000;  // a constant signal: output / kProbe ~= gain

std::vector<int16_t> constantFrames(uint32_t frames, int16_t value) {
  return std::vector<int16_t>(frames * 2, value);
}

// The gain each frame got, measured on a constant full-scale-ish signal.
std::vector<double> gainsOf(const std::vector<int16_t>& out) {
  std::vector<double> g;
  for (size_t i = 0; i < out.size(); i += 2) g.push_back(out[i] / static_cast<double>(kProbe));
  return g;
}

// Runs `frames` frames of the constant signal through `r`.
std::vector<int16_t> run(GainRamp& r, uint32_t frames) {
  auto v = constantFrames(frames, kProbe);
  r.process(v.data(), frames);
  return v;
}

double db(double g) { return 20.0 * std::log10(g); }
}  // namespace

void setUp() {}
void tearDown() {}

// ---- VolumeMath ----

void test_percent_abs_round_trip_is_exact() {
  for (int p = 0; p <= 100; ++p) {
    TEST_ASSERT_EQUAL_UINT8(p, vol::absToPercent(vol::percentToAbs(static_cast<uint8_t>(p))));
  }
  TEST_ASSERT_EQUAL_UINT8(0, vol::percentToAbs(0));
  TEST_ASSERT_EQUAL_UINT8(38, vol::percentToAbs(30));
  TEST_ASSERT_EQUAL_UINT8(127, vol::percentToAbs(100));
  TEST_ASSERT_EQUAL_UINT8(127, vol::percentToAbs(250));  // clamped
  TEST_ASSERT_EQUAL_UINT8(100, vol::absToPercent(127));
  TEST_ASSERT_EQUAL_UINT8(100, vol::absToPercent(200));  // clamped
  for (int a = 1; a <= 127; ++a) {  // never goes down as the absolute volume goes up
    TEST_ASSERT_TRUE(vol::absToPercent(static_cast<uint8_t>(a)) >= vol::absToPercent(static_cast<uint8_t>(a - 1)));
  }
}

void test_headroom_constant_is_minus_2_db() {
  TEST_ASSERT_EQUAL_UINT16(vol::kHeadroomQ15, vol::dbToQ15(vol::kHeadroomDb));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, -2.0f, vol::q15ToDb(vol::kHeadroomQ15));
  TEST_ASSERT_EQUAL_UINT16(32768, vol::dbToQ15(0.0f));
  TEST_ASSERT_EQUAL_UINT16(32768, vol::dbToQ15(6.0f));  // never above unity
}

void test_software_curve() {
  TEST_ASSERT_EQUAL_UINT16(0, vol::softwareVolumeQ15(0));
  TEST_ASSERT_EQUAL_UINT16(32768, vol::softwareVolumeQ15(100));
  TEST_ASSERT_FLOAT_WITHIN(0.05f, -28.0f, vol::q15ToDb(vol::softwareVolumeQ15(30)));
  TEST_ASSERT_FLOAT_WITHIN(0.05f, -20.0f, vol::q15ToDb(vol::softwareVolumeQ15(50)));
  TEST_ASSERT_FLOAT_WITHIN(0.3f, -39.6f, vol::q15ToDb(vol::softwareVolumeQ15(1)));
  for (int p = 1; p <= 100; ++p) {
    TEST_ASSERT_TRUE(vol::softwareVolumeQ15(static_cast<uint8_t>(p)) >
                     vol::softwareVolumeQ15(static_cast<uint8_t>(p - 1)));
  }
}

void test_mul_q15_rounds() {
  TEST_ASSERT_EQUAL_UINT16(32768, vol::mulQ15(32768, 32768));
  TEST_ASSERT_EQUAL_UINT16(26029, vol::mulQ15(26029, 32768));
  TEST_ASSERT_EQUAL_UINT16(0, vol::mulQ15(0, 32768));
  TEST_ASSERT_EQUAL_UINT16(1037, vol::mulQ15(1305, 26029));  // 1037.12
}

// ---- GainRamp: sample arithmetic ----

void test_unity_is_bit_exact() {
  GainRamp g(GainRamp::kUnity);
  std::vector<int16_t> in = {0, 1, -1, 2, -2, 12345, -12345, 32767, -32768, 100};
  std::vector<int16_t> out = in;
  g.process(out.data(), static_cast<uint32_t>(out.size() / 2));
  TEST_ASSERT_EQUAL_INT16_ARRAY(in.data(), out.data(), in.size());
}

void test_scaling_rounds_instead_of_truncating() {
  for (int x = -32768; x <= 32767; x += 97) {
    const int16_t y = GainRamp::scale(static_cast<int16_t>(x), vol::kHeadroomQ15);
    const double exact = x * static_cast<double>(vol::kHeadroomQ15) / 32768.0;
    TEST_ASSERT_TRUE(std::fabs(y - exact) <= 0.5 + 1e-9);
  }
  TEST_ASSERT_EQUAL_INT16(-1, GainRamp::scale(-1, vol::kHeadroomQ15));  // -0.79
  TEST_ASSERT_EQUAL_INT16(1, GainRamp::scale(1, vol::kHeadroomQ15));
  TEST_ASSERT_EQUAL_INT16(3, GainRamp::scale(5, 16384));    // 2.5 -> 3
  TEST_ASSERT_EQUAL_INT16(-2, GainRamp::scale(-5, 16384));  // -2.5 -> -2
  TEST_ASSERT_EQUAL_INT16(32767, GainRamp::scale(32767, GainRamp::kUnity));
  TEST_ASSERT_EQUAL_INT16(-32768, GainRamp::scale(-32768, GainRamp::kUnity));
  TEST_ASSERT_EQUAL_INT16(32767, GainRamp::scale(32767, 2 * GainRamp::kUnity));  // saturates
}

void test_zero_gain_is_silence() {
  GainRamp g(0);
  auto v = run(g, 64);
  for (int16_t s : v) TEST_ASSERT_EQUAL_INT16(0, s);
}

void test_target_is_clamped_to_unity() {
  GainRamp g(GainRamp::kUnity);
  g.request(60000, false);
  auto v = run(g, 8);
  for (int16_t s : v) TEST_ASSERT_EQUAL_INT16(kProbe, s);
  TEST_ASSERT_EQUAL_UINT16(GainRamp::kUnity, g.currentQ15());
}

// ---- GainRamp: movement ----

void test_snap_applies_from_the_next_block() {
  GainRamp g(vol::kHeadroomQ15);
  g.request(1000, true);
  auto v = run(g, 4);
  for (size_t i = 0; i < v.size(); ++i) TEST_ASSERT_EQUAL_INT16(GainRamp::scale(kProbe, 1000), v[i]);
  TEST_ASSERT_EQUAL_UINT16(1000, g.currentQ15());
}

void test_snap_never_jumps_up() {
  GainRamp g(1000);
  g.request(vol::kHeadroomQ15, true);
  run(g, 4);
  TEST_ASSERT_TRUE(g.currentQ15() < 1010);  // ramped, not jumped
}

void test_snap_survives_a_later_target() {
  GainRamp g(vol::kHeadroomQ15);  // the last link was at absolute volume
  g.request(1000, true);           // a new link: down to the software level...
  g.request(1200, false);          // ...then a volume step before any audio
  g.restart();                     // the first stream
  run(g, GainRamp::kFadeFrames);
  // Quickly back only to the snapped level, then slowly on to 1200.
  TEST_ASSERT_TRUE(g.currentQ15() >= 1000 && g.currentQ15() < 1120);  // +0.9 dB in the rest of the 46 ms
}

void test_reset_sets_the_gain_outright() {
  GainRamp g(0);
  g.reset(5000);
  auto v = run(g, 2);
  TEST_ASSERT_EQUAL_INT16(GainRamp::scale(kProbe, 5000), v[0]);
  g.restart();
  g.reset(6000);  // also forgets a pending restart
  v = run(g, 2);
  TEST_ASSERT_EQUAL_INT16(GainRamp::scale(kProbe, 6000), v[0]);
}

void test_down_is_quick_linear_and_exact() {
  GainRamp g(vol::kHeadroomQ15);
  g.request(0, false);
  auto v = run(g, GainRamp::kDownFrames);
  const auto gains = gainsOf(v);
  for (size_t i = 1; i < gains.size(); ++i) TEST_ASSERT_TRUE(gains[i] <= gains[i - 1]);
  // Full scale in kDownFrames: from -2 dB it's silent within ~80 % of that.
  const uint32_t expect = (GainRamp::kDownFrames * vol::kHeadroomQ15 + GainRamp::kUnity - 1) / GainRamp::kUnity;
  TEST_ASSERT_EQUAL_INT16(0, v[2 * (expect - 1)]);
  TEST_ASSERT_TRUE(v[2 * (expect - 2)] > 0);
  TEST_ASSERT_EQUAL_UINT16(0, g.currentQ15());
}

void test_up_never_faster_than_20_db_per_second() {
  // Software 30 % to absolute volume: the handover the headphones' volume takes.
  const uint16_t from = vol::mulQ15(vol::kHeadroomQ15, vol::softwareVolumeQ15(30));  // -30 dB
  GainRamp g(from);
  g.request(vol::kHeadroomQ15, false);
  constexpr uint32_t kWindow = 441;  // 10 ms
  uint32_t frames = 0;
  double last = db(from / 32768.0);
  double worstPerWindow = 0;
  while (g.currentQ15() != vol::kHeadroomQ15 && frames < 10 * 44100) {
    run(g, kWindow);
    frames += kWindow;
    const double now = db(g.currentQ15() / 32768.0);
    worstPerWindow = std::fmax(worstPerWindow, now - last);
    last = now;
  }
  const double seconds = frames / 44100.0;
  const double rise = db(vol::kHeadroomQ15 / 32768.0) - db(from / 32768.0);  // ~28 dB
  TEST_ASSERT_TRUE(seconds >= rise / 20.5);  // no faster than ~20 dB/s overall
  TEST_ASSERT_TRUE(seconds <= rise / 18.0);  // and not needlessly slow
  TEST_ASSERT_TRUE(worstPerWindow <= 0.2 + 0.02);  // 20 dB/s in every 10 ms, +Q15 rounding
}

void test_up_from_silence_gets_going() {
  GainRamp g(0);
  g.request(vol::dbToQ15(-40.0f), false);
  run(g, 2048);
  TEST_ASSERT_TRUE(g.currentQ15() >= 30);  // about -60 dB after 46 ms
  run(g, 44100);
  TEST_ASSERT_EQUAL_UINT16(vol::dbToQ15(-40.0f), g.currentQ15());
}

void test_up_is_monotonic_and_lands_exactly() {
  GainRamp g(8000);
  g.request(vol::kHeadroomQ15, false);
  auto v = run(g, 44100);
  const auto gains = gainsOf(v);
  for (size_t i = 1; i < gains.size(); ++i) TEST_ASSERT_TRUE(gains[i] >= gains[i - 1]);
  TEST_ASSERT_EQUAL_UINT16(vol::kHeadroomQ15, g.currentQ15());
  TEST_ASSERT_EQUAL_INT16(GainRamp::scale(kProbe, vol::kHeadroomQ15), v[v.size() - 2]);
}

void test_restart_fades_back_quickly_to_the_level_heard() {
  GainRamp g(vol::kHeadroomQ15);
  g.restart();
  auto v = run(g, GainRamp::kFadeFrames);
  TEST_ASSERT_TRUE(std::abs(v[0]) < 64);  // starts from silence
  const auto gains = gainsOf(v);
  for (size_t i = 1; i < gains.size(); ++i) TEST_ASSERT_TRUE(gains[i] >= gains[i - 1]);
  TEST_ASSERT_EQUAL_UINT16(vol::kHeadroomQ15, g.currentQ15());  // back within ~46 ms
}

void test_restart_then_higher_target_is_quick_then_slow() {
  const uint16_t heard = vol::dbToQ15(-30.0f);
  GainRamp g(heard);
  g.request(vol::kHeadroomQ15, false);  // e.g. absolute volume arrived while suspended
  g.restart();
  run(g, GainRamp::kFadeFrames);
  // Quickly back to what was heard, but not beyond it by more than the slow rate allows.
  TEST_ASSERT_TRUE(g.currentQ15() >= heard);
  TEST_ASSERT_TRUE(vol::q15ToDb(g.currentQ15()) < -30.0f + 2.0f);
  run(g, 44100);  // one more second: ~20 dB further, not all the way
  TEST_ASSERT_TRUE(g.currentQ15() < vol::kHeadroomQ15);
  TEST_ASSERT_TRUE(vol::q15ToDb(g.currentQ15()) > -12.0f);
}

void test_restart_with_lower_target_fades_to_the_target() {
  GainRamp g(vol::kHeadroomQ15);
  g.request(1000, false);
  g.restart();
  auto v = run(g, 200);
  const auto gains = gainsOf(v);
  for (size_t i = 1; i < gains.size(); ++i) TEST_ASSERT_TRUE(gains[i] >= gains[i - 1]);
  TEST_ASSERT_EQUAL_UINT16(1000, g.currentQ15());
}

void test_snap_before_restart_fades_to_the_snapped_level() {
  GainRamp g(vol::kHeadroomQ15);  // the last link was at absolute volume
  g.request(1000, true);           // a new link snaps to the software level
  g.restart();                     // its first stream starts
  run(g, 100);
  TEST_ASSERT_EQUAL_UINT16(1000, g.currentQ15());
  g.request(vol::kHeadroomQ15, false);  // then the headphones take over: slow
  run(g, 4410);
  TEST_ASSERT_TRUE(vol::q15ToDb(g.currentQ15()) < vol::q15ToDb(1000) + 2.5f);
}

// A handover to the headphones before anything was heard on the link: the
// first stream fades in from silence straight to the new level.
void test_lift_sets_the_level_the_next_restart_fades_to() {
  const uint16_t sw = vol::dbToQ15(-30.0f);
  GainRamp g(vol::kHeadroomQ15);
  g.request(sw, true);            // new link: snapped to the software level
  g.lift(vol::kHeadroomQ15);      // then handed over, still no audio
  g.restart();                    // the first stream
  auto v = run(g, GainRamp::kFadeFrames);
  TEST_ASSERT_TRUE(std::abs(v[0]) < 64);  // from silence
  const auto gains = gainsOf(v);
  for (size_t i = 1; i < gains.size(); ++i) TEST_ASSERT_TRUE(gains[i] >= gains[i - 1]);
  TEST_ASSERT_EQUAL_UINT16(vol::kHeadroomQ15, g.currentQ15());  // within ~46 ms, no swell
}

// Audio already playing: a lift can't raise it; the slow ramp applies.
void test_lift_without_a_restart_is_dropped() {
  const uint16_t sw = vol::dbToQ15(-30.0f);
  GainRamp g(sw);
  run(g, 64);                 // playing
  g.lift(vol::kHeadroomQ15);
  run(g, 64);                 // no restart: dropped, ramps slowly
  TEST_ASSERT_TRUE(vol::q15ToDb(g.currentQ15()) < -30.0f + 0.5f);
  g.restart();                // a later restart must not use the stale lift
  run(g, GainRamp::kFadeFrames);
  TEST_ASSERT_TRUE(vol::q15ToDb(g.currentQ15()) < -30.0f + 2.0f);
}

// A lift whose restart was used up by a stream that started first (the
// headphones started it; the control side didn't know yet): no step, only
// the up rate, from the level playing to the lift's level; the stream's
// next restart then fades back to where the ramp got.
void test_lift_after_the_restart_was_used_up_only_ramps() {
  const uint16_t sw = vol::mulQ15(vol::kHeadroomQ15, vol::softwareVolumeQ15(30));  // -30 dB
  GainRamp g(vol::kHeadroomQ15);
  g.request(sw, true);  // new link: snapped
  g.restart();          // their stream's first data
  run(g, GainRamp::kFadeFrames);
  TEST_ASSERT_EQUAL_UINT16(sw, g.currentQ15());
  g.lift(vol::kHeadroomQ15);  // a handover still taken for a pre-audio one
  constexpr uint32_t kWindow = 441;  // 10 ms
  double last = db(sw / 32768.0);
  uint32_t frames = 0;
  while (g.currentQ15() != vol::kHeadroomQ15 && frames < 10 * 44100) {
    const auto gains = gainsOf(run(g, kWindow));
    for (size_t i = 1; i < gains.size(); ++i) TEST_ASSERT_TRUE(gains[i] >= gains[i - 1]);
    frames += kWindow;
    const double now = db(g.currentQ15() / 32768.0);
    TEST_ASSERT_TRUE(now - last <= 0.2 + 0.02);  // 20 dB/s in every 10 ms, +Q15 rounding
    last = now;
  }
  TEST_ASSERT_EQUAL_UINT16(vol::kHeadroomQ15, g.currentQ15());
  const double rise = db(vol::kHeadroomQ15 / 32768.0) - db(sw / 32768.0);
  TEST_ASSERT_TRUE(frames / 44100.0 >= rise / 20.5);
  // Our own stream later: the quick fade back to the level heard, no further.
  g.restart();
  run(g, GainRamp::kFadeFrames);
  TEST_ASSERT_EQUAL_UINT16(vol::kHeadroomQ15, g.currentQ15());
}

void test_lift_never_exceeds_the_target_and_a_snap_cancels_it() {
  const uint16_t sw = vol::dbToQ15(-30.0f);
  GainRamp g(sw);
  g.lift(vol::kHeadroomQ15);
  g.request(vol::dbToQ15(-20.0f), false);  // e.g. AVRCP lost before the stream: lower target
  g.restart();
  run(g, GainRamp::kFadeFrames);
  TEST_ASSERT_EQUAL_UINT16(vol::dbToQ15(-20.0f), g.currentQ15());

  GainRamp h(sw);
  h.lift(vol::kHeadroomQ15);
  h.request(sw, true);  // a new link after the lift: void
  h.restart();
  run(h, GainRamp::kFadeFrames);
  TEST_ASSERT_EQUAL_UINT16(sw, h.currentQ15());
}

void test_second_restart_during_the_fade_is_ignored() {
  GainRamp g(vol::kHeadroomQ15);
  g.restart();
  auto first = run(g, 512);
  const auto before = gainsOf(first).back();
  g.restart();  // e.g. a flush and a gap both seen for the same new stream
  auto v = run(g, GainRamp::kFadeFrames);
  const auto gains = gainsOf(v);
  TEST_ASSERT_TRUE(gains[0] >= before);  // no drop back to silence
  for (size_t i = 1; i < gains.size(); ++i) TEST_ASSERT_TRUE(gains[i] >= gains[i - 1]);
  TEST_ASSERT_EQUAL_UINT16(vol::kHeadroomQ15, g.currentQ15());
  g.restart();  // once the fade is over, a restart counts again
  TEST_ASSERT_TRUE(std::abs(run(g, 1)[0]) < 64);
}

void test_chunking_does_not_change_the_output() {
  GainRamp a(1037), b(1037);
  a.request(vol::kHeadroomQ15, false);
  b.request(vol::kHeadroomQ15, false);
  a.restart();
  b.restart();
  std::vector<int16_t> in(2 * 70000);
  for (size_t i = 0; i < in.size(); ++i) in[i] = static_cast<int16_t>((i * 7919) % 65536 - 32768);
  std::vector<int16_t> one = in, many = in;
  a.process(one.data(), 70000);
  for (uint32_t done = 0; done < 70000;) {
    const uint32_t n = done + 128 <= 70000 ? 128 : 70000 - done;  // the stack asks for 128
    b.process(many.data() + 2 * done, n);
    done += n;
  }
  TEST_ASSERT_EQUAL_INT16_ARRAY(one.data(), many.data(), one.size());
}

void test_latest_request_wins() {
  GainRamp g(vol::kHeadroomQ15);
  g.request(vol::kHeadroomQ15 / 2, false);  // superseded before any audio
  g.request(100, true);
  auto v = run(g, 2);
  TEST_ASSERT_EQUAL_INT16(GainRamp::scale(kProbe, 100), v[0]);
}

void test_left_and_right_get_the_same_gain() {
  GainRamp g(0);
  g.request(20000, false);
  auto v = constantFrames(300, 12000);
  for (size_t i = 1; i < v.size(); i += 2) v[i] = -12000;
  g.process(v.data(), 300);
  for (size_t i = 0; i < 300; ++i) {
    TEST_ASSERT_TRUE(v[2 * i] == -v[2 * i + 1] || v[2 * i] == -v[2 * i + 1] - 1);
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_percent_abs_round_trip_is_exact);
  RUN_TEST(test_headroom_constant_is_minus_2_db);
  RUN_TEST(test_software_curve);
  RUN_TEST(test_mul_q15_rounds);
  RUN_TEST(test_unity_is_bit_exact);
  RUN_TEST(test_scaling_rounds_instead_of_truncating);
  RUN_TEST(test_zero_gain_is_silence);
  RUN_TEST(test_target_is_clamped_to_unity);
  RUN_TEST(test_snap_applies_from_the_next_block);
  RUN_TEST(test_snap_never_jumps_up);
  RUN_TEST(test_snap_survives_a_later_target);
  RUN_TEST(test_reset_sets_the_gain_outright);
  RUN_TEST(test_down_is_quick_linear_and_exact);
  RUN_TEST(test_up_never_faster_than_20_db_per_second);
  RUN_TEST(test_up_from_silence_gets_going);
  RUN_TEST(test_up_is_monotonic_and_lands_exactly);
  RUN_TEST(test_restart_fades_back_quickly_to_the_level_heard);
  RUN_TEST(test_restart_then_higher_target_is_quick_then_slow);
  RUN_TEST(test_restart_with_lower_target_fades_to_the_target);
  RUN_TEST(test_snap_before_restart_fades_to_the_snapped_level);
  RUN_TEST(test_lift_sets_the_level_the_next_restart_fades_to);
  RUN_TEST(test_lift_without_a_restart_is_dropped);
  RUN_TEST(test_lift_after_the_restart_was_used_up_only_ramps);
  RUN_TEST(test_lift_never_exceeds_the_target_and_a_snap_cancels_it);
  RUN_TEST(test_second_restart_during_the_fade_is_ignored);
  RUN_TEST(test_chunking_does_not_change_the_output);
  RUN_TEST(test_latest_request_wins);
  RUN_TEST(test_left_and_right_get_the_same_gain);
  return UNITY_END();
}
