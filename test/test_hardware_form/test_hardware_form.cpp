#include <unity.h>
#include <map>
#include <string>
#include "hardware_form.h"
#include "config.h"

void setUp(void) {}
void tearDown(void) {}

class MapParams : public FormParams {
 public:
  std::map<std::string, std::string> values;
  bool has(const char* name) const override { return values.count(name) > 0; }
  String get(const char* name, const char* fallback) const override {
    auto it = values.find(name);
    return String(it == values.end() ? fallback : it->second.c_str());
  }
};

static bool contains(const String& s, const char* needle) { return s.indexOf(needle) >= 0; }

// ---- Parsing ----

void test_net_parse_all_fields(void) {
  MapParams p;
  p.values = {{"enabled", "on"}, {"activeLow", "on"}, {"pin", "7"}, {"outageThresholdMinutes", "3"},
              {"pulseSeconds", "20"}};
  NetWatchdogSettings s = parseNetWatchdogForm(p);
  TEST_ASSERT_TRUE(s.enabled);
  TEST_ASSERT_TRUE(s.activeLow);
  TEST_ASSERT_EQUAL_INT(7, s.pin);
  TEST_ASSERT_EQUAL_UINT32(180000, s.outageThresholdMs);
  TEST_ASSERT_EQUAL_UINT32(20000, s.pulseDurationMs);
}

void test_net_parse_empty_form_uses_defaults(void) {
  MapParams p;
  NetWatchdogSettings s = parseNetWatchdogForm(p);
  TEST_ASSERT_FALSE(s.enabled);
  TEST_ASSERT_FALSE(s.activeLow);
  TEST_ASSERT_EQUAL_INT(NET_WATCHDOG_PIN_DEFAULT, s.pin);
  TEST_ASSERT_EQUAL_UINT32(300000, s.outageThresholdMs);
  TEST_ASSERT_EQUAL_UINT32(10000, s.pulseDurationMs);
}

void test_zero_or_negative_durations_become_the_minimum(void) {
  MapParams p;
  p.values = {{"outageThresholdMinutes", "0"}, {"pulseSeconds", "-5"}};
  NetWatchdogSettings s = parseNetWatchdogForm(p);
  TEST_ASSERT_EQUAL_UINT32(60000, s.outageThresholdMs);
  TEST_ASSERT_EQUAL_UINT32(1000, s.pulseDurationMs);
}

void test_durations_are_capped(void) {
  MapParams p;
  p.values = {{"outageThresholdMinutes", "90"}, {"pulseSeconds", "90"}};
  NetWatchdogSettings s = parseNetWatchdogForm(p);
  TEST_ASSERT_EQUAL_UINT32(NET_WATCHDOG_THRESHOLD_MAX_MS, s.outageThresholdMs);
  TEST_ASSERT_EQUAL_UINT32(NET_WATCHDOG_PULSE_MAX_MS, s.pulseDurationMs);
}

// 71583 min * 60000 = 2^32 + 16 on a 32-bit board: without clamping first this
// wrapped to a threshold of almost nothing.
void test_huge_durations_cap_instead_of_wrapping(void) {
  MapParams p;
  p.values = {{"outageThresholdMinutes", "71583"}, {"pulseSeconds", "4294968"}};
  NetWatchdogSettings net = parseNetWatchdogForm(p);
  TEST_ASSERT_EQUAL_UINT32(NET_WATCHDOG_THRESHOLD_MAX_MS, net.outageThresholdMs);
  TEST_ASSERT_EQUAL_UINT32(NET_WATCHDOG_PULSE_MAX_MS, net.pulseDurationMs);
  BridgeWatchdogSettings bridge = parseBridgeWatchdogForm(p);
  TEST_ASSERT_EQUAL_UINT32(BRIDGE_WATCHDOG_THRESHOLD_MAX_MS, bridge.outageThresholdMs);
  TEST_ASSERT_EQUAL_UINT32(BRIDGE_WATCHDOG_PULSE_MAX_MS, bridge.pulseDurationMs);
}

void test_bridge_parse_cameras_and_default_pin(void) {
  MapParams p;
  p.values = {{"enabled", "on"}, {"cameraA", "D01"}, {"cameraB", "D02"}};
  BridgeWatchdogSettings s = parseBridgeWatchdogForm(p);
  TEST_ASSERT_TRUE(s.enabled);
  TEST_ASSERT_EQUAL_STRING("D01", s.cameraA.c_str());
  TEST_ASSERT_EQUAL_STRING("D02", s.cameraB.c_str());
  TEST_ASSERT_EQUAL_INT(BRIDGE_WATCHDOG_PIN_DEFAULT, s.pin);
}

void test_power_parse(void) {
  MapParams p;
  TEST_ASSERT_EQUAL_INT(POWER_MONITOR_PIN_DEFAULT, parsePowerMonitorForm(p).pin);
  p.values = {{"enabled", "on"}, {"activeHigh", "on"}, {"pin", "7"}};
  PowerMonitorSettings s = parsePowerMonitorForm(p);
  TEST_ASSERT_TRUE(s.enabled && s.activeHigh);
  TEST_ASSERT_EQUAL_INT(7, s.pin);
}

// ---- Validation ----

static NetWatchdogSettings net(bool enabled, int pin) { NetWatchdogSettings s; s.enabled = enabled; s.pin = pin; return s; }
static PowerMonitorSettings power(bool enabled, int pin) { PowerMonitorSettings s; s.enabled = enabled; s.pin = pin; return s; }
static BridgeWatchdogSettings bridge(bool enabled, int pin) {
  BridgeWatchdogSettings s;
  s.enabled = enabled;
  s.pin = pin;
  s.cameraA = "A";
  s.cameraB = "B";
  return s;
}

void test_valid_settings_have_no_error(void) {
  TEST_ASSERT_EQUAL_STRING("", validateNetWatchdogSettings(net(true, 4), bridge(true, 5), power(true, 6)).c_str());
  TEST_ASSERT_EQUAL_STRING("", validateBridgeWatchdogSettings(bridge(true, 5), net(true, 4), power(true, 6)).c_str());
  TEST_ASSERT_EQUAL_STRING("", validatePowerMonitorSettings(power(true, 6), net(true, 4), bridge(true, 5)).c_str());
}

void test_reserved_pin_is_rejected_by_all_three(void) {
  int sdPin = SD_CS_PIN;
  TEST_ASSERT_TRUE(contains(validateNetWatchdogSettings(net(true, sdPin), bridge(false, 5), power(false, 6)), "reserved"));
  TEST_ASSERT_TRUE(contains(validateBridgeWatchdogSettings(bridge(true, sdPin), net(false, 4), power(false, 6)), "reserved"));
  TEST_ASSERT_TRUE(contains(validatePowerMonitorSettings(power(true, sdPin), net(false, 4), bridge(false, 5)), "reserved"));
}

void test_disabled_feature_may_keep_a_reserved_pin(void) {
  TEST_ASSERT_EQUAL_STRING("", validateNetWatchdogSettings(net(false, SD_CS_PIN), bridge(false, 5), power(false, 6)).c_str());
}

void test_pin_clash_names_the_other_feature(void) {
  TEST_ASSERT_TRUE(contains(validateNetWatchdogSettings(net(true, 7), bridge(true, 7), power(false, 6)), "Camera Bridge Watchdog"));
  TEST_ASSERT_TRUE(contains(validateNetWatchdogSettings(net(true, 7), bridge(false, 5), power(true, 7)), "220V Power Monitor"));
  TEST_ASSERT_TRUE(contains(validateBridgeWatchdogSettings(bridge(true, 7), net(true, 7), power(false, 6)), "Internet Watchdog"));
  TEST_ASSERT_TRUE(contains(validateBridgeWatchdogSettings(bridge(true, 7), net(false, 4), power(true, 7)), "220V Power Monitor"));
  TEST_ASSERT_TRUE(contains(validatePowerMonitorSettings(power(true, 7), net(true, 7), bridge(false, 5)), "Internet Watchdog"));
  TEST_ASSERT_TRUE(contains(validatePowerMonitorSettings(power(true, 7), net(false, 4), bridge(true, 7)), "Camera Bridge Watchdog"));
}

void test_clash_with_a_disabled_feature_is_fine(void) {
  TEST_ASSERT_EQUAL_STRING("", validateNetWatchdogSettings(net(true, 7), bridge(false, 7), power(false, 7)).c_str());
}

void test_bridge_needs_two_different_cameras(void) {
  BridgeWatchdogSettings s = bridge(true, 5);
  s.cameraB = "";
  TEST_ASSERT_TRUE(contains(validateBridgeWatchdogSettings(s, net(false, 4), power(false, 6)), "must be selected"));
  s.cameraB = "a";  // same as "A", ignoring case
  TEST_ASSERT_TRUE(contains(validateBridgeWatchdogSettings(s, net(false, 4), power(false, 6)), "two different cameras"));
  s.enabled = false;
  TEST_ASSERT_EQUAL_STRING("", validateBridgeWatchdogSettings(s, net(false, 4), power(false, 6)).c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_net_parse_all_fields);
  RUN_TEST(test_net_parse_empty_form_uses_defaults);
  RUN_TEST(test_zero_or_negative_durations_become_the_minimum);
  RUN_TEST(test_durations_are_capped);
  RUN_TEST(test_huge_durations_cap_instead_of_wrapping);
  RUN_TEST(test_bridge_parse_cameras_and_default_pin);
  RUN_TEST(test_power_parse);
  RUN_TEST(test_valid_settings_have_no_error);
  RUN_TEST(test_reserved_pin_is_rejected_by_all_three);
  RUN_TEST(test_disabled_feature_may_keep_a_reserved_pin);
  RUN_TEST(test_pin_clash_names_the_other_feature);
  RUN_TEST(test_clash_with_a_disabled_feature_is_fine);
  RUN_TEST(test_bridge_needs_two_different_cameras);
  return UNITY_END();
}
