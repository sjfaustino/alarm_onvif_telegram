#pragma once
#include <Arduino.h>
#include <vector>

// Pure WiFi scan post-processing, tested natively.

struct WifiScanResult {
  String ssid;
  int32_t rssi = 0;
  bool encrypted = false;
};

// One entry per SSID (strongest AP wins), strongest first; hidden (blank)
// SSIDs dropped.
std::vector<WifiScanResult> dedupeSortWifiScanResults(const std::vector<WifiScanResult>& raw);
