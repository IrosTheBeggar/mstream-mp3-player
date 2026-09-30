#pragma once
#include <cstdint>

// Which headphones the Core2 may look for and connect to by itself, before
// any are remembered. Pairing is the listener's: Output > Pair new
// headphones lists the audio devices in pairing mode with their kind and
// signal, and connects only to the one tapped. Nothing else ever picks a
// device for them:
//   - Remembered headphones are paged, never scanned for (an inquiry can't
//     find them unless they are in pairing mode): no scan while any are.
//   - With none remembered, a scan (inquiry) runs only to look for a name
//     given at build time (BT_SINK_NAME, developer builds; the console's
//     c<name> changes it there), and never after the Output tab's Forget
//     (the forgotten ones must not come back by their name). A release
//     build has no name: it never scans by itself. The Core2 stays quiet,
//     connectable only.
//   - A device a scan finds is taken only by that name. Never by signal
//     strength alone: once a TV in the next room was paired that way
//     (docs/POC-RESULTS.md). The one exception is the console's Bs, for
//     developers: RAM only, off at boot, logged, for one scan.
// Portable: BtSink (src/audio) feeds it and carries it out; host-tested
// (test_sink_search), with ReconnectPlanner's use of it (test_reconnect).
namespace sinksearch {

// Bs's "close enough" (dBm): about the Core2 touching the headphones. -70
// wasn't enough: the TV in the next room fluctuated past it.
inline constexpr int kMinRssi = -55;

struct Setup {
  const char* name = "";       // the name to look for: a build's BT_SINK_NAME, "" none
  bool forgotForGood = false;  // the Output tab's Forget, until new headphones link
  bool bySignal = false;       // the console's Bs: the next scan takes a device this close
};

// Whether a scan by name may run: none remembered, and a name to look for
// (not forgotten), or Bs armed.
bool mayScan(bool remembered, const Setup& s);

// What a scan's find is (BtSink::onDeviceFound: audio sinks only).
enum class Verdict : uint8_t {
  ByName,     // its name contains the name looked for (case-insensitive): taken
  BySignal,   // Bs armed, and it is at kMinRssi or closer: taken
  NoName,     // no name to look for (a release build): never taken
  OtherName,  // another name
  Forgotten,  // the name matches, but the listener forgot the headphones
  TooFar,     // Bs armed, not close enough (and not the name)
};
Verdict judge(const Setup& s, const char* found, int rssi);
inline bool taken(Verdict v) { return v == Verdict::ByName || v == Verdict::BySignal; }
// Why it isn't taken, for the log ("" when it is).
const char* whyNot(Verdict v);

// `needle` in `haystack`, ignoring ASCII case; false for an empty needle.
bool containsIgnoreCase(const char* haystack, const char* needle);

}  // namespace sinksearch
