#include "wifi_scan.h"
#include <algorithm>

std::vector<WifiScanResult> dedupeSortWifiScanResults(const std::vector<WifiScanResult>& raw) {
  std::vector<WifiScanResult> out;
  for (auto& r : raw) {
    if (r.ssid.length() == 0) continue; // hidden network - nothing to prefill

    bool merged = false;
    for (auto& existing : out) {
      if (existing.ssid == r.ssid) {
        if (r.rssi > existing.rssi) existing = r; // stronger AP for the same network wins
        merged = true;
        break;
      }
    }
    if (!merged) out.push_back(r);
  }

  std::sort(out.begin(), out.end(),
            [](const WifiScanResult& a, const WifiScanResult& b) { return a.rssi > b.rssi; });
  return out;
}
