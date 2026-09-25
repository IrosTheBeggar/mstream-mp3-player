#pragma once
#include <cstdint>

// What the Bluetooth heartbeat (ESP32-A2DP's, every 10 s) should do about
// finding the headphones while no link is up. Portable, so the reconnect and
// discovery cycle can be tested on the host; BtSink's PlayerA2dp maps the
// library's state in and carries the decision out.
//
// The library alone gets stuck: its unconnected handler only pages the
// remembered headphones while auto-reconnect is armed, and it isn't after a
// first connection that failed on a fresh NVS, nor once its retries ran out
// and a connection found by scanning failed. And while it scans it never
// pages the remembered headphones again, which aren't discoverable unless
// pairing. The cycle this gives: page the remembered headphones (the
// library's retries), scan for kScanForMs, page them again, and so on; with
// none remembered, scan.
class ReconnectPlanner {
public:
  enum class Lib : uint8_t {
    Unconnected,  // the library's UNCONNECTED (CONNECTED without a link counts as this)
    Connecting,   // paging; the library times it out after 2 heartbeats
    Discovering,  // scanning
    Other,        // DISCOVERED (connecting to a device found) and the rest
  };

  struct In {
    bool linked;           // an A2DP link is up
    Lib state;
    bool discoveryActive;  // the stack reports a scan running
    bool armed;            // auto-reconnect allowed and AutoReconnect (what the library pages on)
    bool remembered;       // a device address is remembered
    uint32_t nowMs;
  };

  enum class Do : uint8_t {
    KeepAlive,        // linked: only note the heartbeat (the library would start an idle stream)
    KeepScanning,     // leave the scan running; nothing else
    StopScanAndPage,  // cancel the scan, state UNCONNECTED, re-arm, then the library's heartbeat
    ReArm,            // re-arm auto-reconnect (full retries), then the library's heartbeat
    Scan,             // state DISCOVERING and start a scan
    PassOn,           // the library's heartbeat as it is
  };

  static constexpr uint32_t kScanForMs = 60000;

  Do heartbeat(const In& in);

private:
  bool scanning_ = false;
  uint32_t scanningSinceMs_ = 0;
};
