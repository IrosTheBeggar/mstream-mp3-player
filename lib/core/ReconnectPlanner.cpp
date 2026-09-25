#include "ReconnectPlanner.h"

ReconnectPlanner::Do ReconnectPlanner::heartbeat(const In& in) {
  if (in.linked) {
    scanning_ = false;
    return Do::KeepAlive;
  }
  if (in.state == Lib::Discovering) {
    if (!scanning_) {
      scanning_ = true;
      scanningSinceMs_ = in.nowMs;
    }
    // Headphones aren't discoverable unless pairing: scanning alone may never
    // find the remembered ones again. Page them now and then.
    if (!in.remembered || in.nowMs - scanningSinceMs_ < kScanForMs) return Do::KeepScanning;
    scanning_ = false;
    return Do::StopScanAndPage;
  }
  scanning_ = false;
  // Not armed: the library's unconnected handler would do nothing, for good.
  if (in.state == Lib::Unconnected && !in.discoveryActive && !in.armed) {
    return in.remembered ? Do::ReArm : Do::Scan;
  }
  return Do::PassOn;
}
