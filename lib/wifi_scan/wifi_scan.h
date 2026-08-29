#pragma once
#include <Arduino.h>
#include <vector>

// Pure post-processing of a raw WiFi scan result list, split out of
// webserver_network.cpp so it can be unit-tested natively
// (test/test_wifi_scan) without WiFi.h, which only exists on-device.

struct WifiScanResult {
  String ssid;
  int32_t rssi = 0;
  bool encrypted = false;
};

// Collapses duplicate SSIDs down to one entry each - several access points
// or mesh nodes broadcasting the same network name is normal and would
// otherwise list "HomeWiFi" three times - keeping whichever one has the
// strongest signal (the one actually worth connecting through), then
// sorts the result strongest-first, the order that's actually useful when
// picking which network to add. A blank SSID (a hidden network WiFi.SSID()
// returns as "") is dropped entirely - there'd be nothing for the Add
// button to usefully prefill.
std::vector<WifiScanResult> dedupeSortWifiScanResults(const std::vector<WifiScanResult>& raw);
