#pragma once
#include <cstdint>

// The Core2's power chip (AXP192) as numbers, for measuring what the
// firmware costs (src/app/PowerProbe, the console's P): its ADC registers
// decoded into one Sample, and the mean/min/max of samples over a window.
// Portable, host-tested; the I2C reads are the caller's.
//
// Which input USB arrives on isn't assumed: the Core2 routes USB-C and the
// M-Bus 5 V to the AXP192's ACIN and VBUS pins, and with the 5 V boost
// (EXTEN) on, the bus's own 5 V can show on VBUS without any current. Both
// are read; "in" is their sum (the one without a supply reads ~0 mA).
namespace power {

struct Sample {
  float acinV = 0.0f, acinMa = 0.0f;  // ACIN, regs 0x56-0x59: 1.7 mV, 0.625 mA per LSB (12 bits)
  float vbusV = 0.0f, vbusMa = 0.0f;  // VBUS, regs 0x5A-0x5D: 1.7 mV, 0.375 mA per LSB (12 bits)
  float tempC = 0.0f;                 // the chip's temperature, 0x5E-0x5F: 0.1 C per LSB, -144.7 C at 0
  float batV = 0.0f;                  // battery voltage, 0x78-0x79: 1.1 mV per LSB (12 bits)
  float batMa = 0.0f;                 // charge (0x7A) minus discharge (0x7C), 0.5 mA per LSB (13 bits):
                                      //   + charging, - discharging (the drain on battery)
  float apsV = 0.0f;                  // APS (IPSOUT, the system rail), 0x7E-0x7F: 1.4 mV per LSB (12 bits)
  uint8_t status = 0;                 // reg 0x00: bit 7 ACIN present, bit 5 VBUS present, bit 2 charging

  float inMa() const { return acinMa + vbusMa; }
  float inW() const { return (acinV * acinMa + vbusV * vbusMa) / 1000.0f; }
  float batW() const { return batV * batMa / 1000.0f; }
};

// Registers 0x56-0x5F (10 bytes: ACIN V, ACIN I, VBUS V, VBUS I,
// temperature) and 0x78-0x7F (8 bytes: battery V, charge I, discharge I,
// APS V), as burst-read from the chip, and reg 0x00.
Sample decode(uint8_t status, const uint8_t in[10], const uint8_t bat[8]);

// The ADC sample rate from reg 0x84 (bits 7-6: 25, 50, 100, 200 Hz).
uint32_t adcHz(uint8_t reg84);

// The coulomb counter (regs 0xB0-0xB7) in mAh: 65536 x 0.5 mA x count /
// 3600 / the ADC rate (AXP192 datasheet, "Coulomb counter").
double coulombMah(uint32_t count, uint32_t adcHz);

struct Stat {
  float mean = 0.0f, min = 0.0f, max = 0.0f;
};

// Samples since reset(): mean/min/max of the input current and power and
// the battery current, means of the rest.
class Window {
public:
  void reset(uint32_t startMs);
  void add(const Sample& s);

  uint32_t count() const { return n_; }
  uint32_t startMs() const { return startMs_; }

  Stat inMa() const { return inMa_.stat(n_); }
  Stat inW() const { return inW_.stat(n_); }
  Stat batMa() const { return batMa_.stat(n_); }
  float batW() const { return batW_.stat(n_).mean; }
  float acinV() const { return acinV_.stat(n_).mean; }
  float acinMa() const { return acinMa_.stat(n_).mean; }
  float vbusV() const { return vbusV_.stat(n_).mean; }
  float vbusMa() const { return vbusMa_.stat(n_).mean; }
  float batV() const { return batV_.stat(n_).mean; }
  float apsV() const { return apsV_.stat(n_).mean; }
  float tempC() const { return tempC_.stat(n_).mean; }
  // reg 0x00 of every sample OR-ed (a supply that came or went shows).
  uint8_t status() const { return status_; }

private:
  struct Acc {
    double sum = 0.0;
    float min = 0.0f, max = 0.0f;
    void add(float v, bool first);
    Stat stat(uint32_t n) const;
  };

  uint32_t startMs_ = 0;
  uint32_t n_ = 0;
  uint8_t status_ = 0;
  Acc inMa_, inW_, batMa_, batW_, acinV_, acinMa_, vbusV_, vbusMa_, batV_, apsV_, tempC_;
};

}  // namespace power
