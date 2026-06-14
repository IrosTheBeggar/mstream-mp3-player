#include "net/HttpProbeDiscovery.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>

namespace {
constexpr uint32_t kProbeIntervalMs = 5000;
}

void HttpProbeDiscovery::parseUrl() {
  if (parsed_) return;
  String url = baseUrl_.c_str();
  scheme_ = "http";
  String rest = url;
  const int s = url.indexOf("://");
  if (s >= 0) {
    scheme_ = std::string(url.substring(0, s).c_str());
    rest = url.substring(s + 3);
  }
  const int slash = rest.indexOf('/');
  if (slash >= 0) rest = rest.substring(0, slash);
  const int colon = rest.indexOf(':');
  if (colon >= 0) {
    host_ = std::string(rest.substring(0, colon).c_str());
    port_ = static_cast<uint16_t>(rest.substring(colon + 1).toInt());
  } else {
    host_ = std::string(rest.c_str());
    port_ = (scheme_ == "https") ? 443 : 80;
  }
  parsed_ = true;
}

void HttpProbeDiscovery::begin() { parseUrl(); }

std::vector<ServerCandidate> HttpProbeDiscovery::poll(uint32_t nowMs) {
  if (!browsing_ || WiFi.status() != WL_CONNECTED) return cached_;
  if (!firstProbe_ && (nowMs - lastProbeMs_) < kProbeIntervalMs) return cached_;
  firstProbe_ = false;
  lastProbeMs_ = nowMs;
  parseUrl();

  std::vector<ServerCandidate> found;
  HTTPClient http;
  const String probeUrl = String(baseUrl_.c_str()) + "/api/";
  http.setConnectTimeout(2000);
  http.setTimeout(3000);
  if (http.begin(probeUrl)) {
    const int code = http.GET();
    if (code == 200) {
      const String body = http.getString();
      // mStream's public GET /api/ returns {"server":"<version>", ...}.
      std::string version;
      const int k = body.indexOf("\"server\"");
      if (k >= 0) {
        const int colon = body.indexOf(':', k);
        const int q1 = body.indexOf('"', colon);
        const int q2 = (q1 >= 0) ? body.indexOf('"', q1 + 1) : -1;
        if (colon >= 0 && q1 >= 0 && q2 > q1) {
          version = std::string(body.substring(q1 + 1, q2).c_str());
        }
      }
      ServerCandidate srv;
      srv.instanceId = "probe:" + baseUrl_;  // stable, distinct from mDNS ids
      srv.instanceName = host_;
      srv.host = host_;
      srv.port = port_;
      srv.scheme = scheme_;
      srv.baseUrl = baseUrl_;
      srv.version = version;
      found.push_back(srv);
      Serial.printf("[probe] %s reachable (mStream %s)\n", baseUrl_.c_str(), version.c_str());
    } else {
      Serial.printf("[probe] %s -> HTTP %d\n", probeUrl.c_str(), code);
    }
    http.end();
  } else {
    Serial.printf("[probe] begin failed for %s\n", probeUrl.c_str());
  }

  cached_ = found;
  return cached_;
}
