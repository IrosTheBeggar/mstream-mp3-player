// Host unit tests for power::decode / power::Window (the AXP192's ADC
// registers, and their statistics for the console's P line).
// Run: pio test -e native
#include <unity.h>

#include "PowerWindow.h"

void setUp() {}
void tearDown() {}

namespace {
// A 12-bit ADC value as the chip lays it out: 8 high bits, then 4 low bits.
void put12(uint8_t* b, uint32_t v) {
  b[0] = static_cast<uint8_t>(v >> 4);
  b[1] = static_cast<uint8_t>(v & 0x0F);
}
// 13 bits: 8 high, 5 low.
void put13(uint8_t* b, uint32_t v) {
  b[0] = static_cast<uint8_t>(v >> 5);
  b[1] = static_cast<uint8_t>(v & 0x1F);
}

struct Regs {
  uint8_t in[10] = {};
  uint8_t bat[8] = {};
};

Regs regs(uint32_t acinV, uint32_t acinI, uint32_t vbusV, uint32_t vbusI, uint32_t temp, uint32_t batV,
          uint32_t charge, uint32_t discharge, uint32_t aps) {
  Regs r;
  put12(r.in + 0, acinV);
  put12(r.in + 2, acinI);
  put12(r.in + 4, vbusV);
  put12(r.in + 6, vbusI);
  put12(r.in + 8, temp);
  put12(r.bat + 0, batV);
  put13(r.bat + 2, charge);
  put13(r.bat + 4, discharge);
  put12(r.bat + 6, aps);
  return r;
}
}  // namespace

void test_decode_scales_every_register() {
  // ACIN 5.1 V (3000 x 1.7 mV) at 312.5 mA (500 x 0.625), VBUS idle,
  // 45.3 C ((1900 x 0.1) - 144.7), battery 4.18 V (3800 x 1.1 mV),
  // charging 100 mA, APS 4.9 V (3500 x 1.4 mV).
  const Regs r = regs(3000, 500, 0, 0, 1900, 3800, 200, 0, 3500);
  const power::Sample s = power::decode(0xA4, r.in, r.bat);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 5.1f, s.acinV);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 312.5f, s.acinMa);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, s.vbusMa);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 45.3f, s.tempC);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 4.18f, s.batV);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 100.0f, s.batMa);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 4.9f, s.apsV);
  TEST_ASSERT_EQUAL_HEX8(0xA4, s.status);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 312.5f, s.inMa());
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 5.1f * 0.3125f, s.inW());
}

void test_decode_discharge_is_negative_and_ignores_reserved_bits() {
  // On battery: 180 mA out (360 x 0.5). The low bytes' unused high bits
  // are masked off (they read as whatever the chip leaves there).
  Regs r = regs(0, 0, 2941, 8, 0, 3500, 0, 360, 2600);
  r.bat[5] |= 0xE0;  // discharge low byte: only 5 bits are data
  r.in[7] |= 0xF0;   // VBUS current low byte: only 4 bits
  const power::Sample s = power::decode(0x00, r.in, r.bat);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, -180.0f, s.batMa);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 3.0f, s.vbusMa);  // 8 x 0.375
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 3.85f, s.batV);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, -0.693f, s.batW());
}

void test_decode_full_scale() {
  const Regs r = regs(4095, 4095, 4095, 4095, 4095, 4095, 8191, 8191, 4095);
  const power::Sample s = power::decode(0, r.in, r.bat);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 2559.375f, s.acinMa);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 1535.625f, s.vbusMa);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, s.batMa);  // 4095.5 in, 4095.5 out
}

void test_window_mean_min_max() {
  power::Window w;
  w.reset(1000);
  TEST_ASSERT_EQUAL_UINT32(0, w.count());
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, w.inMa().mean);  // empty: zeros, not NaN
  const float in[] = {300.0f, 250.0f, 350.0f, 300.0f};
  for (float ma : in) {
    power::Sample s;
    s.acinV = 5.0f;
    s.acinMa = ma;
    s.batV = 4.0f;
    s.batMa = -ma / 2;
    s.status = ma > 320.0f ? 0x80 : 0x00;
    w.add(s);
  }
  TEST_ASSERT_EQUAL_UINT32(4, w.count());
  TEST_ASSERT_EQUAL_UINT32(1000, w.startMs());
  const power::Stat i = w.inMa();
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 300.0f, i.mean);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 250.0f, i.min);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 350.0f, i.max);
  const power::Stat p = w.inW();
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.5f, p.mean);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.25f, p.min);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.75f, p.max);
  const power::Stat b = w.batMa();
  TEST_ASSERT_FLOAT_WITHIN(0.001f, -150.0f, b.mean);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, -175.0f, b.min);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, -125.0f, b.max);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, -0.6f, w.batW());
  TEST_ASSERT_EQUAL_HEX8(0x80, w.status());

  w.reset(9000);  // a fresh window forgets the old min/max
  power::Sample s;
  s.acinMa = 500.0f;
  w.add(s);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 500.0f, w.inMa().min);
  TEST_ASSERT_EQUAL_UINT32(9000, w.startMs());
  TEST_ASSERT_EQUAL_HEX8(0x00, w.status());
}

void test_adc_rate_and_coulombs() {
  TEST_ASSERT_EQUAL_UINT32(25, power::adcHz(0x32));  // M5Unified's setting
  TEST_ASSERT_EQUAL_UINT32(50, power::adcHz(0x72));
  TEST_ASSERT_EQUAL_UINT32(100, power::adcHz(0xB2));
  TEST_ASSERT_EQUAL_UINT32(200, power::adcHz(0xF2));
  // 65536 x 0.5 / 3600 / 25 = 0.36409 mAh per count at 25 Hz.
  TEST_ASSERT_FLOAT_WITHIN(1e-4, 0.36409, power::coulombMah(1, 25));
  TEST_ASSERT_FLOAT_WITHIN(1e-3, 364.089, power::coulombMah(1000, 25));
  TEST_ASSERT_FLOAT_WITHIN(1e-3, 45.511, power::coulombMah(1000, 200));
  TEST_ASSERT_FLOAT_WITHIN(1e-9, 0.0, power::coulombMah(5, 0));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_decode_scales_every_register);
  RUN_TEST(test_decode_discharge_is_negative_and_ignores_reserved_bits);
  RUN_TEST(test_decode_full_scale);
  RUN_TEST(test_window_mean_min_max);
  RUN_TEST(test_adc_rate_and_coulombs);
  return UNITY_END();
}
