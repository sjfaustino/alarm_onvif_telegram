#include <unity.h>
#include <Arduino.h>
#include "wifi_scan.h"

void setUp(void) {}
void tearDown(void) {}

void test_empty_input_returns_empty(void) {
  std::vector<WifiScanResult> out = dedupeSortWifiScanResults({});
  TEST_ASSERT_EQUAL(0, out.size());
}

void test_sorts_strongest_signal_first(void) {
  std::vector<WifiScanResult> raw = {
      {"Weak", -80, false},
      {"Strong", -40, true},
      {"Medium", -60, false},
  };
  std::vector<WifiScanResult> out = dedupeSortWifiScanResults(raw);
  TEST_ASSERT_EQUAL(3, out.size());
  TEST_ASSERT_EQUAL_STRING("Strong", out[0].ssid.c_str());
  TEST_ASSERT_EQUAL_STRING("Medium", out[1].ssid.c_str());
  TEST_ASSERT_EQUAL_STRING("Weak", out[2].ssid.c_str());
}

void test_duplicate_ssid_keeps_strongest_signal(void) {
  // Two APs/mesh nodes broadcasting the same network name - a normal
  // setup this must not list twice.
  std::vector<WifiScanResult> raw = {
      {"HomeWiFi", -75, true},
      {"HomeWiFi", -45, true},
      {"HomeWiFi", -60, true},
  };
  std::vector<WifiScanResult> out = dedupeSortWifiScanResults(raw);
  TEST_ASSERT_EQUAL(1, out.size());
  TEST_ASSERT_EQUAL(-45, out[0].rssi);
}

void test_hidden_network_with_blank_ssid_is_dropped(void) {
  std::vector<WifiScanResult> raw = {{"", -50, true}, {"Visible", -55, false}};
  std::vector<WifiScanResult> out = dedupeSortWifiScanResults(raw);
  TEST_ASSERT_EQUAL(1, out.size());
  TEST_ASSERT_EQUAL_STRING("Visible", out[0].ssid.c_str());
}

void test_encrypted_flag_preserved(void) {
  std::vector<WifiScanResult> raw = {{"Open", -50, false}, {"Locked", -50, true}};
  std::vector<WifiScanResult> out = dedupeSortWifiScanResults(raw);
  TEST_ASSERT_EQUAL(2, out.size());
  for (auto& r : out) {
    if (r.ssid == "Open") TEST_ASSERT_FALSE(r.encrypted);
    if (r.ssid == "Locked") TEST_ASSERT_TRUE(r.encrypted);
  }
}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_empty_input_returns_empty);
  RUN_TEST(test_sorts_strongest_signal_first);
  RUN_TEST(test_duplicate_ssid_keeps_strongest_signal);
  RUN_TEST(test_hidden_network_with_blank_ssid_is_dropped);
  RUN_TEST(test_encrypted_flag_preserved);
  return UNITY_END();
}
