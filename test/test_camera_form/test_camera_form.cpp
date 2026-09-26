#include <unity.h>
#include "fixtures.h"
#include "golden.h"
#include "config.h"

void setUp(void) {}
void tearDown(void) {}

// ---- Rendering: byte-identical to the captured pre-refactor output ----

void test_render_add_default(void) {
  TEST_ASSERT_EQUAL_STRING(kGoldenAddDefault, renderCameraForm(fixtureAddDefault(), false).c_str());
}

void test_render_edit_hostile(void) {
  TEST_ASSERT_EQUAL_STRING(kGoldenEditHostile, renderCameraForm(fixtureEditHostile(), true).c_str());
}

void test_render_add_hostile(void) {
  TEST_ASSERT_EQUAL_STRING(kGoldenAddHostile, renderCameraForm(fixtureEditHostile(), false).c_str());
}

void test_render_wrap_no_warning(void) {
  TEST_ASSERT_EQUAL_STRING(kGoldenWrapNoWarning, renderCameraForm(fixtureWrapNoWarning(), true).c_str());
}

void test_render_wrap_warning(void) {
  TEST_ASSERT_EQUAL_STRING(kGoldenWrapWarning, renderCameraForm(fixtureWrapWarning(), true).c_str());
}

void test_render_never_echoes_password(void) {
  String html = renderCameraForm(fixtureEditHostile(), true);
  TEST_ASSERT_EQUAL(-1, html.indexOf("secret"));
}

// ---- Parsing ----

void test_parse_empty_form_uses_defaults(void) {
  MapParams p;
  CameraConfig c = parseCameraForm(p);
  CameraConfig d;
  TEST_ASSERT_EQUAL_STRING("", c.name.c_str());
  TEST_ASSERT_FALSE(c.enabled);  // unchecked boxes are simply absent
  TEST_ASSERT_FALSE(c.useWSSecurity);
  TEST_ASSERT_FALSE(c.personAlertsEnabled);
  TEST_ASSERT_EQUAL_UINT32(30000, c.alertCooldownMs);
  TEST_ASSERT_EQUAL_UINT32(5UL * 60000UL, c.offlineThresholdMs);
  TEST_ASSERT_EQUAL_UINT(1, c.snapshotBurstCount);
  TEST_ASSERT_EQUAL_UINT32(2000, c.pollIntervalMs);
  TEST_ASSERT_EQUAL_UINT16(0, c.quietStartMinute);
  TEST_ASSERT_EQUAL_UINT16(0, c.motionWatchdogHours);
  TEST_ASSERT_EQUAL_UINT16(0, c.retentionDays);
  TEST_ASSERT_EQUAL_UINT16(d.snapshotMaxWidth, c.snapshotMaxWidth);
}

void test_parse_all_fields(void) {
  MapParams p;
  p.values = {{"name", "  Gate  "},
              {"deviceServiceUrl", "http://x/onvif"},
              {"user", "u"},
              {"pass", "p"},
              {"notes", "n"},
              {"snapshotUriOverride", "http://x/s"},
              {"preferredProfileKeyword", "sub"},
              {"enabled", "on"},
              {"useWSSecurity", "on"},
              {"includeInitialTerminationTime", "on"},
              {"includeReplyToAnonymous", "on"},
              {"quietHoursEnabled", "on"},
              {"timelapseSendToTelegram", "on"},
              {"personAlertsEnabled", "on"},
              {"vehicleAlertsEnabled", "on"},
              {"petAlertsEnabled", "on"},
              {"petAlertsTextOnly", "on"},
              {"motionDigestEnabled", "on"},
              {"alertCooldownSec", "45"},
              {"offlineThresholdMin", "10"},
              {"snapshotBurstCount", "3"},
              {"quietStart", "22:15"},
              {"quietEnd", "06:00"},
              {"motionWatchdogHours", "12"},
              {"timelapseIntervalMin", "30"},
              {"retentionDays", "60"},
              {"snapshotMaxWidth", "640"},
              {"snapshotMaxHeight", "480"},
              {"pollIntervalMs", "1500"}};
  CameraConfig c = parseCameraForm(p);
  TEST_ASSERT_EQUAL_STRING("Gate", c.name.c_str());  // trimmed
  TEST_ASSERT_EQUAL_STRING("http://x/onvif", c.deviceServiceUrl.c_str());
  TEST_ASSERT_EQUAL_STRING("u", c.user.c_str());
  TEST_ASSERT_EQUAL_STRING("p", c.pass.c_str());
  TEST_ASSERT_EQUAL_STRING("n", c.notes.c_str());
  TEST_ASSERT_EQUAL_STRING("http://x/s", c.snapshotUriOverride.c_str());
  TEST_ASSERT_EQUAL_STRING("sub", c.preferredProfileKeyword.c_str());
  TEST_ASSERT_TRUE(c.enabled);
  TEST_ASSERT_TRUE(c.useWSSecurity);
  TEST_ASSERT_TRUE(c.includeInitialTerminationTime);
  TEST_ASSERT_TRUE(c.includeReplyToAnonymous);
  TEST_ASSERT_TRUE(c.quietHoursEnabled);
  TEST_ASSERT_TRUE(c.timelapseSendToTelegram);
  TEST_ASSERT_TRUE(c.personAlertsEnabled);
  TEST_ASSERT_TRUE(c.vehicleAlertsEnabled);
  TEST_ASSERT_TRUE(c.petAlertsEnabled);
  TEST_ASSERT_TRUE(c.petAlertsTextOnly);
  TEST_ASSERT_TRUE(c.motionDigestEnabled);
  TEST_ASSERT_EQUAL_UINT32(45000, c.alertCooldownMs);
  TEST_ASSERT_EQUAL_UINT32(600000, c.offlineThresholdMs);
  TEST_ASSERT_EQUAL_UINT(3, c.snapshotBurstCount);
  TEST_ASSERT_EQUAL_UINT16(22 * 60 + 15, c.quietStartMinute);
  TEST_ASSERT_EQUAL_UINT16(6 * 60, c.quietEndMinute);
  TEST_ASSERT_EQUAL_UINT16(12, c.motionWatchdogHours);
  TEST_ASSERT_EQUAL_UINT16(30, c.timelapseIntervalMin);
  TEST_ASSERT_EQUAL_UINT16(60, c.retentionDays);
  TEST_ASSERT_EQUAL_UINT16(640, c.snapshotMaxWidth);
  TEST_ASSERT_EQUAL_UINT16(480, c.snapshotMaxHeight);
  TEST_ASSERT_EQUAL_UINT32(1500, c.pollIntervalMs);
}

void test_parse_zero_or_negative_fall_back_to_defaults(void) {
  MapParams p;
  p.values = {{"alertCooldownSec", "0"}, {"offlineThresholdMin", "-3"}, {"snapshotBurstCount", "0"},
              {"pollIntervalMs", "0"}};
  CameraConfig c = parseCameraForm(p);
  CameraConfig d;
  TEST_ASSERT_EQUAL_UINT32(d.alertCooldownMs, c.alertCooldownMs);
  TEST_ASSERT_EQUAL_UINT32(d.offlineThresholdMs, c.offlineThresholdMs);
  TEST_ASSERT_EQUAL_UINT(1, c.snapshotBurstCount);
  TEST_ASSERT_EQUAL_UINT32(d.pollIntervalMs, c.pollIntervalMs);
}

void test_parse_clamps_upper_bounds(void) {
  MapParams p;
  p.values = {{"alertCooldownSec", "99999999"}, {"offlineThresholdMin", "99999999"},
              {"snapshotBurstCount", "500"},    {"motionWatchdogHours", "1000"},
              {"timelapseIntervalMin", "9999"}, {"retentionDays", "99999"},
              {"snapshotMaxWidth", "99999"},    {"snapshotMaxHeight", "99999"},
              {"pollIntervalMs", "99999999"}};
  CameraConfig c = parseCameraForm(p);
  TEST_ASSERT_EQUAL_UINT32(CAMERA_ALERT_COOLDOWN_MAX_MS, c.alertCooldownMs);
  TEST_ASSERT_EQUAL_UINT32(CAMERA_OFFLINE_THRESHOLD_MAX_MS, c.offlineThresholdMs);
  TEST_ASSERT_EQUAL_UINT(CAMERA_SNAPSHOT_BURST_MAX, c.snapshotBurstCount);
  TEST_ASSERT_EQUAL_UINT16(168, c.motionWatchdogHours);
  TEST_ASSERT_EQUAL_UINT16(1440, c.timelapseIntervalMin);
  TEST_ASSERT_EQUAL_UINT16(SD_RETENTION_MAX_DAYS, c.retentionDays);
  TEST_ASSERT_EQUAL_UINT16(CAMERA_SNAPSHOT_DIMENSION_MAX, c.snapshotMaxWidth);
  TEST_ASSERT_EQUAL_UINT16(CAMERA_SNAPSHOT_DIMENSION_MAX, c.snapshotMaxHeight);
  TEST_ASSERT_EQUAL_UINT32(CAMERA_POLL_INTERVAL_MAX_MS, c.pollIntervalMs);
}

void test_parse_clamps_lower_bounds(void) {
  MapParams p;
  p.values = {{"motionWatchdogHours", "-5"}, {"timelapseIntervalMin", "-5"}, {"retentionDays", "-5"},
              {"snapshotMaxWidth", "-5"},    {"snapshotMaxHeight", "-5"},    {"pollIntervalMs", "10"}};
  CameraConfig c = parseCameraForm(p);
  TEST_ASSERT_EQUAL_UINT16(0, c.motionWatchdogHours);
  TEST_ASSERT_EQUAL_UINT16(0, c.timelapseIntervalMin);
  TEST_ASSERT_EQUAL_UINT16(0, c.retentionDays);
  TEST_ASSERT_EQUAL_UINT16(0, c.snapshotMaxWidth);
  TEST_ASSERT_EQUAL_UINT16(0, c.snapshotMaxHeight);
  TEST_ASSERT_EQUAL_UINT32(CAMERA_POLL_INTERVAL_MIN_MS, c.pollIntervalMs);  // raised, not discarded
}

void test_parse_malformed_quiet_times_are_midnight(void) {
  MapParams p;
  p.values = {{"quietStart", "7:30"}, {"quietEnd", "24:00"}};
  CameraConfig c = parseCameraForm(p);
  TEST_ASSERT_EQUAL_UINT16(0, c.quietStartMinute);
  TEST_ASSERT_EQUAL_UINT16(0, c.quietEndMinute);
}

// ---- HH:MM helpers ----

void test_hhmm_round_trip(void) {
  TEST_ASSERT_EQUAL_STRING("00:00", minutesToHHMM(0).c_str());
  TEST_ASSERT_EQUAL_STRING("23:59", minutesToHHMM(1439).c_str());
  TEST_ASSERT_EQUAL_UINT16(1439, parseHHMMToMinutes("23:59"));
  TEST_ASSERT_EQUAL_UINT16(0, parseHHMMToMinutes("ab:cd"));
  TEST_ASSERT_EQUAL_UINT16(0, parseHHMMToMinutes("12:60"));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_render_add_default);
  RUN_TEST(test_render_edit_hostile);
  RUN_TEST(test_render_add_hostile);
  RUN_TEST(test_render_wrap_no_warning);
  RUN_TEST(test_render_wrap_warning);
  RUN_TEST(test_render_never_echoes_password);
  RUN_TEST(test_parse_empty_form_uses_defaults);
  RUN_TEST(test_parse_all_fields);
  RUN_TEST(test_parse_zero_or_negative_fall_back_to_defaults);
  RUN_TEST(test_parse_clamps_upper_bounds);
  RUN_TEST(test_parse_clamps_lower_bounds);
  RUN_TEST(test_parse_malformed_quiet_times_are_midnight);
  RUN_TEST(test_hhmm_round_trip);
  return UNITY_END();
}
