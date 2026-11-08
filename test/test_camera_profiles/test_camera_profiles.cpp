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

void test_matching_ignores_case_and_whitespace(void) {
  const CameraModelProfile* p = findCameraModelProfile(" vatilon ", "pb1");
  TEST_ASSERT_NOT_NULL(p);
  TEST_ASSERT_EQUAL_STRING("Vatilon", p->manufacturer);
}

void test_specific_model_wins_over_manufacturer_wide_entry(void) {
  const CameraModelProfile* h46 = findCameraModelProfile("Vatilon", "H46");
  const CameraModelProfile* h44 = findCameraModelProfile("Vatilon", "H44");
  TEST_ASSERT_NOT_NULL(h46);
  TEST_ASSERT_NOT_NULL(h44);
  TEST_ASSERT_EQUAL_STRING("H46", h46->model);
  TEST_ASSERT_EQUAL_STRING("", h44->model);
}

void test_unknown_and_empty_identities_match_nothing(void) {
  TEST_ASSERT_NULL(findCameraModelProfile("Hikvision", "DS-2CD2043"));
  TEST_ASSERT_NULL(findCameraModelProfile("", ""));
  // A model-specific entry doesn't match other models from the maker.
  TEST_ASSERT_NULL(findCameraModelProfile("REOLINK", "E1 Zoom"));
}

void test_vatilon_fills_snapshot_url_on_the_device_host_and_poll_interval(void) {
  CameraConfig c = newCamera("http://192.168.8.231/onvif/device_service");
  std::vector<String> changes = applyCameraModelProfile(c, *findCameraModelProfile("Vatilon", "PB1"));
  TEST_ASSERT_EQUAL_STRING("http://192.168.8.231/cgi-bin/snapshot.cgi", c.snapshotUriOverride.c_str());
  TEST_ASSERT_EQUAL_UINT32(500, c.pollIntervalMs);
  TEST_ASSERT_EQUAL(2, (int)changes.size());
  TEST_ASSERT_EQUAL_STRING("", c.notes.c_str());
}

void test_snapshot_url_keeps_the_port(void) {
  CameraConfig c = newCamera("http://192.168.10.220:8080/onvif/device_service");
  applyCameraModelProfile(c, *findCameraModelProfile("Vatilon", "H44"));
  TEST_ASSERT_EQUAL_STRING("http://192.168.10.220:8080/cgi-bin/snapshot.cgi", c.snapshotUriOverride.c_str());
}

void test_values_the_user_typed_are_kept(void) {
  CameraConfig c = newCamera("http://192.168.8.231/onvif/device_service");
  c.snapshotUriOverride = "http://192.168.8.231/custom.jpg";
  c.pollIntervalMs = 1000;
  std::vector<String> changes = applyCameraModelProfile(c, *findCameraModelProfile("Vatilon", "PB1"));
  TEST_ASSERT_EQUAL_STRING("http://192.168.8.231/custom.jpg", c.snapshotUriOverride.c_str());
  TEST_ASSERT_EQUAL_UINT32(1000, c.pollIntervalMs);
  TEST_ASSERT_EQUAL(0, (int)changes.size());
}

void test_reolink_turns_on_initial_termination_time(void) {
  CameraConfig c = newCamera("http://192.168.8.80:8000/onvif/device_service");
  std::vector<String> changes = applyCameraModelProfile(c, *findCameraModelProfile("REOLINK", "RLC-520A"));
  TEST_ASSERT_TRUE(c.includeInitialTerminationTime);
  TEST_ASSERT_EQUAL(1, (int)changes.size());
  TEST_ASSERT_EQUAL_STRING("", c.snapshotUriOverride.c_str());
  TEST_ASSERT_EQUAL_UINT32(CameraConfig().pollIntervalMs, c.pollIntervalMs);
}

void test_tip_is_appended_to_notes_once(void) {
  CameraConfig c = newCamera("http://192.168.10.222/onvif/device_service");
  c.notes = "Container rear.";
  const CameraModelProfile& p = *findCameraModelProfile("Vatilon", "H46");
  applyCameraModelProfile(c, p);
  TEST_ASSERT_TRUE(c.notes.startsWith("Container rear. "));
  TEST_ASSERT_TRUE(c.notes.indexOf("Snapshot Setting") > 0);
  String once = c.notes;
  std::vector<String> again = applyCameraModelProfile(c, p);
  TEST_ASSERT_EQUAL_STRING(once.c_str(), c.notes.c_str());
  TEST_ASSERT_EQUAL(0, (int)again.size());
}

void test_tip_only_entry_changes_no_settings(void) {
  CameraConfig c = newCamera("http://192.168.8.120:2020/onvif/device_service");
  std::vector<String> changes = applyCameraModelProfile(c, *findCameraModelProfile("tp-link", "Tapo TC65"));
  TEST_ASSERT_EQUAL(1, (int)changes.size());
  TEST_ASSERT_EQUAL_STRING("", c.snapshotUriOverride.c_str());
  TEST_ASSERT_TRUE(c.notes.length() > 0);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_matching_ignores_case_and_whitespace);
  RUN_TEST(test_specific_model_wins_over_manufacturer_wide_entry);
  RUN_TEST(test_unknown_and_empty_identities_match_nothing);
  RUN_TEST(test_vatilon_fills_snapshot_url_on_the_device_host_and_poll_interval);
  RUN_TEST(test_snapshot_url_keeps_the_port);
  RUN_TEST(test_values_the_user_typed_are_kept);
  RUN_TEST(test_reolink_turns_on_initial_termination_time);
  RUN_TEST(test_tip_is_appended_to_notes_once);
  RUN_TEST(test_tip_only_entry_changes_no_settings);
  return UNITY_END();
}
