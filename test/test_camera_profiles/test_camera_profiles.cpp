#include <unity.h>
#include "camera_profiles.h"

void setUp(void) {}
void tearDown(void) {}

static CameraConfig newCamera(const char* url) {
  CameraConfig c;
  c.name = "D30";
  c.deviceServiceUrl = url;
  return c;
}

static const uint32_t kLan = (192u << 24) | (168u << 16) | (8u << 8) | 132u;
static const uint32_t kMask24 = 0xFFFFFF00u;

void test_matching_ignores_case_and_whitespace(void) {
  TEST_ASSERT_TRUE(findCameraModelProfile(" vatilon ", "pb1").known);
}

void test_h46_gets_the_manufacturer_settings_and_its_own_tip(void) {
  CameraModelProfile h46 = findCameraModelProfile("Vatilon", "H46");
  CameraModelProfile h44 = findCameraModelProfile("Vatilon", "H44");
  TEST_ASSERT_EQUAL_STRING("/cgi-bin/snapshot.cgi", h46.snapshotPath);
  TEST_ASSERT_EQUAL_UINT32(h44.pollIntervalMs, h46.pollIntervalMs);
  TEST_ASSERT_NOT_NULL(h46.tip);
  TEST_ASSERT_NULL(h44.tip);
}

void test_unknown_and_empty_identities_match_nothing(void) {
  TEST_ASSERT_FALSE(findCameraModelProfile("Hikvision", "DS-2CD2043").known);
  TEST_ASSERT_FALSE(findCameraModelProfile("", "").known);
  // A model-specific entry doesn't match other models from the maker.
  TEST_ASSERT_FALSE(findCameraModelProfile("REOLINK", "E1 Zoom").known);
}

void test_vatilon_fills_snapshot_url_on_the_device_host_and_poll_interval(void) {
  CameraConfig c = newCamera("http://192.168.8.231/onvif/device_service");
  std::vector<String> changes = applyCameraModelProfile(c, findCameraModelProfile("Vatilon", "PB1"), true);
  TEST_ASSERT_EQUAL_STRING("http://192.168.8.231/cgi-bin/snapshot.cgi", c.snapshotUriOverride.c_str());
  TEST_ASSERT_EQUAL_UINT32(500, c.pollIntervalMs);
  TEST_ASSERT_EQUAL(2, (int)changes.size());
  TEST_ASSERT_EQUAL_STRING("", c.notes.c_str());
}

void test_camera_off_the_local_subnet_keeps_the_default_poll(void) {
  CameraConfig c = newCamera("http://192.168.10.220/onvif/device_service");
  std::vector<String> changes = applyCameraModelProfile(c, findCameraModelProfile("Vatilon", "PB1"), false);
  TEST_ASSERT_EQUAL_UINT32(CameraConfig().pollIntervalMs, c.pollIntervalMs);
  TEST_ASSERT_EQUAL_STRING("http://192.168.10.220/cgi-bin/snapshot.cgi", c.snapshotUriOverride.c_str());
  TEST_ASSERT_EQUAL(1, (int)changes.size());
}

void test_snapshot_url_keeps_the_port(void) {
  CameraConfig c = newCamera("http://192.168.10.220:8080/onvif/device_service");
  applyCameraModelProfile(c, findCameraModelProfile("Vatilon", "H44"), true);
  TEST_ASSERT_EQUAL_STRING("http://192.168.10.220:8080/cgi-bin/snapshot.cgi", c.snapshotUriOverride.c_str());
}

void test_values_the_user_typed_are_kept(void) {
  CameraConfig c = newCamera("http://192.168.8.231/onvif/device_service");
  c.snapshotUriOverride = "http://192.168.8.231/custom.jpg";
  c.pollIntervalMs = 1000;
  std::vector<String> changes = applyCameraModelProfile(c, findCameraModelProfile("Vatilon", "PB1"), true);
  TEST_ASSERT_EQUAL_STRING("http://192.168.8.231/custom.jpg", c.snapshotUriOverride.c_str());
  TEST_ASSERT_EQUAL_UINT32(1000, c.pollIntervalMs);
  TEST_ASSERT_EQUAL(0, (int)changes.size());
}

void test_reolink_turns_on_initial_termination_time(void) {
  CameraConfig c = newCamera("http://192.168.8.80:8000/onvif/device_service");
  std::vector<String> changes = applyCameraModelProfile(c, findCameraModelProfile("REOLINK", "RLC-520A"), true);
  TEST_ASSERT_TRUE(c.includeInitialTerminationTime);
  TEST_ASSERT_EQUAL(1, (int)changes.size());
  TEST_ASSERT_EQUAL_STRING("", c.snapshotUriOverride.c_str());
  TEST_ASSERT_EQUAL_UINT32(CameraConfig().pollIntervalMs, c.pollIntervalMs);
}

void test_tip_is_appended_to_notes_once(void) {
  CameraConfig c = newCamera("http://192.168.10.222/onvif/device_service");
  c.notes = "Container rear.";
  CameraModelProfile p = findCameraModelProfile("Vatilon", "H46");
  applyCameraModelProfile(c, p, true);
  TEST_ASSERT_TRUE(c.notes.startsWith("Container rear. "));
  TEST_ASSERT_TRUE(c.notes.indexOf("Snapshot Setting") > 0);
  String once = c.notes;
  std::vector<String> again = applyCameraModelProfile(c, p, true);
  TEST_ASSERT_EQUAL_STRING(once.c_str(), c.notes.c_str());
  TEST_ASSERT_EQUAL(0, (int)again.size());
}

void test_tip_only_entry_changes_no_settings(void) {
  CameraConfig c = newCamera("http://192.168.8.120:2020/onvif/device_service");
  std::vector<String> changes = applyCameraModelProfile(c, findCameraModelProfile("tp-link", "Tapo TC65"), true);
  TEST_ASSERT_EQUAL(1, (int)changes.size());
  TEST_ASSERT_EQUAL_STRING("", c.snapshotUriOverride.c_str());
  TEST_ASSERT_TRUE(c.notes.length() > 0);
}

void test_host_on_subnet(void) {
  TEST_ASSERT_TRUE(hostIsOnSubnet("192.168.8.231", kLan, kMask24));
  TEST_ASSERT_TRUE(hostIsOnSubnet("192.168.8.104:8000", kLan, kMask24));
  TEST_ASSERT_FALSE(hostIsOnSubnet("192.168.10.220", kLan, kMask24));
  TEST_ASSERT_FALSE(hostIsOnSubnet("192.168.10.220:80", kLan, kMask24));
}

void test_anything_but_an_ipv4_address_is_not_local(void) {
  TEST_ASSERT_FALSE(hostIsOnSubnet("camera.local", kLan, kMask24));
  TEST_ASSERT_FALSE(hostIsOnSubnet("", kLan, kMask24));
  TEST_ASSERT_FALSE(hostIsOnSubnet("192.168.8", kLan, kMask24));
  TEST_ASSERT_FALSE(hostIsOnSubnet("192.168.8.1.5", kLan, kMask24));
  TEST_ASSERT_FALSE(hostIsOnSubnet("192.168.8.999", kLan, kMask24));
  // Without the range check, 256 would carry into the octet before it and match.
  TEST_ASSERT_FALSE(hostIsOnSubnet("192.168.7.256", kLan, kMask24));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_matching_ignores_case_and_whitespace);
  RUN_TEST(test_h46_gets_the_manufacturer_settings_and_its_own_tip);
  RUN_TEST(test_unknown_and_empty_identities_match_nothing);
  RUN_TEST(test_vatilon_fills_snapshot_url_on_the_device_host_and_poll_interval);
  RUN_TEST(test_camera_off_the_local_subnet_keeps_the_default_poll);
  RUN_TEST(test_snapshot_url_keeps_the_port);
  RUN_TEST(test_values_the_user_typed_are_kept);
  RUN_TEST(test_reolink_turns_on_initial_termination_time);
  RUN_TEST(test_tip_is_appended_to_notes_once);
  RUN_TEST(test_tip_only_entry_changes_no_settings);
  RUN_TEST(test_host_on_subnet);
  RUN_TEST(test_anything_but_an_ipv4_address_is_not_local);
  return UNITY_END();
}
